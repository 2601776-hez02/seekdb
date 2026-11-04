#!/usr/bin/env python3
# -*- coding: ascii -*-
"""Transparent AI gateway for seekdb's AI functions.

The gateway sits between the database and a model provider (or a mock model
service).  It has three jobs:

1. pass requests through unchanged (it is a transparent reverse proxy);
2. measure real usage per model: input/output tokens, latency, status and
   repeated (retried) requests;
3. inject faults on purpose: fixed or distributed delay, HTTP 429/500,
   connection reset, and "upstream answered but the response is lost".

It is a standalone developer/test tool.  It is NOT part of the database and
therefore NOT part of the transaction boundary: the database commits a row
first and the worker calls the model afterwards, so the gateway can see a
request that the database later decides to discard (see README.md).

Standard library only.  No model credentials are needed: whatever access key
the database sends is forwarded verbatim to the upstream.

Usage:

    python3 gateway.py --config gateway.json
    python3 gateway.py --upstream http://127.0.0.1:19120 --port 19121
    python3 gateway.py --port 19121 --print-usage      # client mode
    python3 gateway.py --port 19121 --reset-usage      # client mode

Config file (JSON, every key optional):

    {
      "upstream": "http://127.0.0.1:19120",
      "listen_host": "127.0.0.1",
      "listen_port": 19121,
      "timeout_ms": 30000,
      "delay_ms": 0,
      "delay_distribution": {"type": "uniform", "min_ms": 10, "max_ms": 50},
      "fail_status": 0,
      "fail_times": 0,
      "fail_rate": 0.0,
      "fail_forever": false,
      "reset_times": 0,
      "reset_rate": 0.0,
      "loss_times": 0,
      "loss_rate": 0.0,
      "prices": {
        "currency": "USD",
        "per_1k_input": 0.0015,
        "per_1k_output": 0.006,
        "models": {"mock-complete": {"per_1k_input": 0.002, "per_1k_output": 0.008}}
      }
    }
"""

import argparse
import hashlib
import json
import os
import random
import socket
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_CONFIG = {
    "upstream": "",
    "listen_host": "127.0.0.1",
    "listen_port": 19121,
    "timeout_ms": 30000,
    "delay_ms": 0,
    "delay_distribution": None,
    "fail_status": 0,
    "fail_times": 0,
    "fail_rate": 0.0,
    "fail_forever": False,
    "reset_times": 0,
    "reset_rate": 0.0,
    "loss_times": 0,
    "loss_rate": 0.0,
    "seed": 0,
    "prices": {
        "currency": "USD",
        "per_1k_input": 0.0,
        "per_1k_output": 0.0,
        "models": {},
    },
}

TOKEN_CHARS = 4.0          # the heuristic: one token per four characters
MAX_LATENCY_SAMPLES = 5000
HOP_BY_HOP_HEADERS = frozenset([
    "host", "content-length", "connection", "keep-alive", "transfer-encoding",
    "te", "trailer", "upgrade", "proxy-authorization", "proxy-authenticate",
    "accept-encoding",
])


# --------------------------------------------------------------------------
# config
# --------------------------------------------------------------------------


def load_config(path=None, overrides=None):
    """Return a validated config merged from defaults, file and overrides."""
    config = json.loads(json.dumps(DEFAULT_CONFIG))  # deep copy
    if path:
        with open(path, "r") as handle:
            file_config = json.load(handle)
        if not isinstance(file_config, dict):
            raise ValueError("config file must contain a JSON object")
        merge_config(config, file_config)
    if overrides:
        merge_config(config, overrides)
    validate_config(config)
    return config


def merge_config(config, patch):
    for key, value in patch.items():
        if value is None:
            continue
        if key == "prices" and isinstance(value, dict):
            prices = config.setdefault("prices", {})
            for price_key, price_value in value.items():
                if price_key == "models" and isinstance(price_value, dict):
                    models = prices.setdefault("models", {})
                    for model_name, model_price in price_value.items():
                        models.setdefault(model_name, {}).update(model_price)
                else:
                    prices[price_key] = price_value
        else:
            config[key] = value


def validate_config(config):
    if not config.get("upstream"):
        raise ValueError("upstream is required (e.g. http://127.0.0.1:19120)")
    for key in ("delay_ms", "fail_times", "reset_times", "loss_times", "seed"):
        config[key] = int(config[key])
        if config[key] < 0 and key != "seed":
            raise ValueError("%s must be >= 0" % key)
    config["fail_status"] = int(config["fail_status"])
    if config["fail_status"] and not 100 <= config["fail_status"] <= 599:
        raise ValueError("fail_status must be 0 or in [100, 599]")
    for key in ("fail_rate", "reset_rate", "loss_rate"):
        config[key] = float(config[key])
        if not 0.0 <= config[key] <= 1.0:
            raise ValueError("%s must be in [0.0, 1.0]" % key)
    config["listen_port"] = int(config["listen_port"])
    config["timeout_ms"] = int(config["timeout_ms"])
    config["fail_forever"] = bool(config["fail_forever"])
    prices = config.get("prices") or {}
    prices["per_1k_input"] = float(prices.get("per_1k_input", 0.0))
    prices["per_1k_output"] = float(prices.get("per_1k_output", 0.0))
    prices.setdefault("currency", "USD")
    prices.setdefault("models", {})
    config["prices"] = prices
    distribution = config.get("delay_distribution")
    if distribution:
        if distribution.get("type") != "uniform":
            raise ValueError("delay_distribution.type must be 'uniform'")
        if float(distribution.get("min_ms", 0)) > float(distribution.get("max_ms", 0)):
            raise ValueError("delay_distribution.min_ms must be <= max_ms")
    return config


def public_config(config):
    keys = ("upstream", "listen_host", "listen_port", "delay_ms",
            "delay_distribution", "fail_status", "fail_times", "fail_rate",
            "fail_forever", "reset_times", "reset_rate", "loss_times",
            "loss_rate", "prices")
    return {key: config.get(key) for key in keys}


# --------------------------------------------------------------------------
# request / response inspection
# --------------------------------------------------------------------------


def estimate_tokens(text):
    """The documented heuristic: one token per four characters, minimum 1.

    The division rounds up: 1-4 characters are one token, 5-8 are two.
    """
    if not text:
        return 0
    return max(1, (len(text) + int(TOKEN_CHARS) - 1) // int(TOKEN_CHARS))


def parse_json(raw):
    if not raw:
        return {}
    try:
        payload = json.loads(raw.decode("utf-8", "replace"))
    except ValueError:
        return {}
    return payload if isinstance(payload, dict) else {}


def request_text(body):
    """Return the prompt/input text of an OpenAI or Ollama style request."""
    if not body:
        return ""
    payload = parse_json(body)
    if not payload:
        return body.decode("utf-8", "replace")
    parts = []
    messages = payload.get("messages")
    if isinstance(messages, list):
        for message in messages:
            if not isinstance(message, dict):
                continue
            content = message.get("content")
            if isinstance(content, str):
                parts.append(content)
            elif isinstance(content, list):
                for item in content:
                    if isinstance(item, dict) and isinstance(item.get("text"), str):
                        parts.append(item["text"])
    inputs = payload.get("input")
    if isinstance(inputs, str):
        parts.append(inputs)
    elif isinstance(inputs, list):
        for item in inputs:
            if isinstance(item, str):
                parts.append(item)
            elif isinstance(item, dict) and isinstance(item.get("text"), str):
                parts.append(item["text"])
    if not parts:
        return body.decode("utf-8", "replace")
    return "\n".join(parts)


def body_model(body):
    payload = parse_json(body)
    model = payload.get("model")
    return model if isinstance(model, str) else ""


def response_text(body):
    """Return the assistant text of an OpenAI or Ollama style response."""
    payload = parse_json(body)
    choices = payload.get("choices")
    if isinstance(choices, list) and choices:
        first = choices[0]
        if isinstance(first, dict):
            message = first.get("message")
            if isinstance(message, dict) and isinstance(message.get("content"), str):
                return message["content"]
            if isinstance(first.get("text"), str):
                return first["text"]
    message = payload.get("message")
    if isinstance(message, dict) and isinstance(message.get("content"), str):
        return message["content"]
    return ""


def retry_key(body):
    """Identity of a request for retry detection: digest of model + input."""
    text = body_model(body) + "\x00" + request_text(body)
    return hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()


# --------------------------------------------------------------------------
# usage accounting
# --------------------------------------------------------------------------


class Usage(object):
    """Thread safe usage counters.

    Two levels are counted on purpose:

    * ``requests`` / ``retries`` are counted for every request the gateway
      receives, including the ones it answers itself (injected 429/500) or
      resets.  That is what "how often did the database call the model" means
      from the gateway point of view.
    * ``upstream_calls`` / ``input_tokens`` / ``output_tokens`` are counted
      only for requests that really reached the upstream, i.e. the ones a real
      provider would bill.  A lost response is still counted here, because the
      upstream did answer.
    """

    def __init__(self):
        self.lock = threading.Lock()
        self.reset()

    def reset(self):
        with self.lock:
            self.requests = 0
            self.upstream_calls = 0
            self.injected_failures = 0
            self.injected_resets = 0
            self.upstream_errors = 0
            self.lost_responses = 0
            self.input_tokens = 0
            self.output_tokens = 0
            self.retries = 0
            self.latencies = []
            self.by_model = {}
            self.seen = {}

    def model_entry(self, model):
        entry = self.by_model.get(model)
        if entry is None:
            entry = {
                "requests": 0,
                "upstream_calls": 0,
                "input_tokens": 0,
                "output_tokens": 0,
                "failures": 0,
                "retries": 0,
                "latency_ms_total": 0,
                "latency_ms_max": 0,
            }
            self.by_model[model] = entry
        return entry

    def add_request(self, model, key):
        with self.lock:
            self.requests += 1
            entry = self.model_entry(model)
            entry["requests"] += 1
            seen = self.seen.get(key, 0)
            if seen > 0:
                self.retries += 1
                entry["retries"] += 1
            self.seen[key] = seen + 1

    def add_upstream(self, model, input_tokens, output_tokens, latency_ms):
        with self.lock:
            self.upstream_calls += 1
            self.input_tokens += input_tokens
            self.output_tokens += output_tokens
            entry = self.model_entry(model)
            entry["upstream_calls"] += 1
            entry["input_tokens"] += input_tokens
            entry["output_tokens"] += output_tokens
            entry["latency_ms_total"] += latency_ms
            if latency_ms > entry["latency_ms_max"]:
                entry["latency_ms_max"] = latency_ms
            self.latencies.append(latency_ms)
            if len(self.latencies) > MAX_LATENCY_SAMPLES:
                del self.latencies[0]

    def add_failure(self, model):
        with self.lock:
            self.injected_failures += 1
            self.model_entry(model)["failures"] += 1

    def add_reset(self, model):
        with self.lock:
            self.injected_resets += 1
            self.model_entry(model)["failures"] += 1

    def add_upstream_error(self, model):
        with self.lock:
            self.upstream_errors += 1
            self.model_entry(model)["failures"] += 1

    def add_loss(self, model):
        with self.lock:
            self.lost_responses += 1
            self.model_entry(model)  # make sure the model shows up

    # -- reporting ------------------------------------------------------

    def percentiles(self):
        with self.lock:
            samples = sorted(self.latencies)
        if not samples:
            return {"avg": 0.0, "max": 0, "p50": 0, "p95": 0}
        count = len(samples)
        return {
            "avg": round(float(sum(samples)) / count, 3),
            "max": samples[-1],
            "p50": samples[0 if count == 1 else int(count * 0.50)],
            "p95": samples[min(count - 1, int(count * 0.95))],
        }

    def cost_for(self, prices, model, input_tokens, output_tokens):
        model_prices = (prices.get("models") or {}).get(model) or {}
        per_input = float(model_prices.get("per_1k_input",
                                          prices.get("per_1k_input", 0.0)))
        per_output = float(model_prices.get("per_1k_output",
                                            prices.get("per_1k_output", 0.0)))
        return (input_tokens / 1000.0) * per_input + \
               (output_tokens / 1000.0) * per_output

    def snapshot(self, config):
        prices = config.get("prices") or {}
        with self.lock:
            total_in = self.input_tokens
            total_out = self.output_tokens
            models = {}
            total_cost = 0.0
            for name in sorted(self.by_model):
                entry = dict(self.by_model[name])
                cost = self.cost_for(prices, name, entry["input_tokens"],
                                     entry["output_tokens"])
                entry["estimated_cost"] = round(cost, 12)
                calls = entry["upstream_calls"]
                entry["latency_ms_avg"] = round(
                    float(entry["latency_ms_total"]) / calls, 3) if calls else 0
                models[name] = entry
                total_cost += cost
            counters = {
                "requests": self.requests,
                "upstream_calls": self.upstream_calls,
                "injected_failures": self.injected_failures,
                "injected_resets": self.injected_resets,
                "upstream_errors": self.upstream_errors,
                "lost_responses": self.lost_responses,
                "retries": self.retries,
                "input_tokens": total_in,
                "output_tokens": total_out,
                "total_tokens": total_in + total_out,
            }
        counters["estimated_cost"] = round(total_cost, 12)
        return {
            "ok": True,
            "currency": prices.get("currency", "USD"),
            "totals": counters,
            "latency_ms": self.percentiles(),
            "by_model": models,
            "config": public_config(config),
        }


# --------------------------------------------------------------------------
# fault injection
# --------------------------------------------------------------------------


class Injector(object):
    """Consumes the configured faults.  One instance per gateway."""

    def __init__(self, config):
        self.lock = threading.Lock()
        self.config = config
        self.random = random.Random(config.get("seed", 0))

    def delay_ms(self):
        distribution = self.config.get("delay_distribution")
        fixed = int(self.config.get("delay_ms") or 0)
        if not distribution:
            return fixed
        low = float(distribution.get("min_ms", 0))
        high = float(distribution.get("max_ms", 0))
        with self.lock:
            sampled = self.random.uniform(low, high)
        return fixed + int(sampled)

    def take_fail(self):
        """Return the HTTP status to answer with, or 0 when the request passes."""
        with self.lock:
            status = int(self.config.get("fail_status") or 0)
            if not status:
                return 0
            if self.config.get("fail_forever"):
                return status
            if self.config.get("fail_times", 0) > 0:
                self.config["fail_times"] -= 1
                return status
            rate = float(self.config.get("fail_rate") or 0.0)
            if rate > 0.0 and self.random.random() < rate:
                return status
            return 0

    def take_reset(self):
        with self.lock:
            if self.config.get("reset_times", 0) > 0:
                self.config["reset_times"] -= 1
                return True
            rate = float(self.config.get("reset_rate") or 0.0)
            return rate > 0.0 and self.random.random() < rate

    def take_loss(self):
        with self.lock:
            if self.config.get("loss_times", 0) > 0:
                self.config["loss_times"] -= 1
                return True
            rate = float(self.config.get("loss_rate") or 0.0)
            return rate > 0.0 and self.random.random() < rate


# --------------------------------------------------------------------------
# HTTP handling
# --------------------------------------------------------------------------


def forward_request(method, path, headers, body, upstream, timeout_ms):
    """Forward one request unchanged and return (status, headers, body)."""
    url = upstream.rstrip("/") + path
    request = urllib.request.Request(url, data=body if method != "GET" else None,
                                     method=method)
    for name in headers.keys():
        if name.lower() in HOP_BY_HOP_HEADERS:
            continue
        request.add_header(name, headers.get(name))
    timeout = max(0.001, timeout_ms / 1000.0)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return (response.getcode(),
                    {key.lower(): value for key, value in response.headers.items()},
                    response.read())
    except urllib.error.HTTPError as exc:
        return (exc.code,
                {key.lower(): value for key, value in (exc.headers or {}).items()},
                exc.read())


class GatewayHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "ai-gateway/1.0"

    config = None
    usage = None
    injector = None
    upstream = ""

    def log_message(self, fmt, *args):
        if os.environ.get("AI_GATEWAY_VERBOSE"):
            sys.stderr.write("gateway: " + (fmt % args) + "\n")

    # -- plumbing -------------------------------------------------------

    def read_body(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        if length <= 0:
            return b""
        return self.rfile.read(length)

    def send_json(self, status, payload):
        raw = json.dumps(payload).encode("utf-8")
        self.send_raw(status, raw, "application/json")

    def send_raw(self, status, raw, content_type=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type or "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)
        self.wfile.flush()

    def close_without_reply(self):
        """Close the client connection without writing any response."""
        self.close_connection = True
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.connection.close()
        except OSError:
            pass

    # -- routes ---------------------------------------------------------

    def do_GET(self):
        if self.path.rstrip("/") == "/__usage":
            self.send_json(200, self.usage.snapshot(self.config))
        elif self.path.rstrip("/") == "/__health":
            self.send_json(200, {"ok": True, "upstream": self.upstream})
        else:
            self.forward()

    def do_POST(self):
        if self.path.rstrip("/") == "/__usage/reset":
            self.read_body()
            self.usage.reset()
            self.send_json(200, {"ok": True, "reset": True})
            return
        self.forward()

    # -- proxy ----------------------------------------------------------

    def forward(self):
        body = self.read_body()
        model = body_model(body) or "unknown"
        key = retry_key(body)
        input_tokens = estimate_tokens(request_text(body))
        self.usage.add_request(model, key)

        delay_ms = self.injector.delay_ms()
        if delay_ms > 0:
            time.sleep(delay_ms / 1000.0)

        fail_status = self.injector.take_fail()
        if fail_status:
            # The request never reaches the model provider: no tokens are spent.
            self.usage.add_failure(model)
            self.send_json(fail_status, {
                "error": {
                    "message": "gateway injected failure",
                    "type": "gateway_injected",
                    "code": fail_status,
                }
            })
            return

        if self.injector.take_reset():
            self.usage.add_reset(model)
            self.close_without_reply()
            return

        started = time.time()
        try:
            status, headers, raw = forward_request(
                self.command, self.path, self.headers, body,
                self.upstream, self.config.get("timeout_ms", 30000))
        except (urllib.error.URLError, socket.error, OSError):
            self.usage.add_upstream_error(model)
            self.send_json(502, {
                "error": {
                    "message": "upstream unavailable",
                    "type": "gateway_upstream",
                    "code": 502,
                }
            })
            return
        latency_ms = int((time.time() - started) * 1000)
        output_tokens = estimate_tokens(response_text(raw)) if status == 200 else 0
        # The upstream answered, therefore the call is billable, even when the
        # response is dropped below or when the database later discards the
        # result because its source row changed.
        self.usage.add_upstream(model, input_tokens, output_tokens, latency_ms)

        if self.injector.take_loss():
            self.usage.add_loss(model)
            self.close_without_reply()
            return

        self.send_raw(status, raw, headers.get("content-type"))


# --------------------------------------------------------------------------
# server wrapper
# --------------------------------------------------------------------------


class Gateway(object):
    """Small wrapper so that tests can start and stop a gateway in process."""

    def __init__(self, config):
        self.config = validate_config(json.loads(json.dumps(config)))
        self.usage = Usage()
        self.injector = Injector(self.config)

        class Handler(GatewayHandler):
            pass

        Handler.config = self.config
        Handler.usage = self.usage
        Handler.injector = self.injector
        Handler.upstream = self.config["upstream"]
        self.handler = Handler
        self.server = ThreadingHTTPServer(
            (self.config.get("listen_host") or "127.0.0.1",
             int(self.config["listen_port"])),
            Handler)
        self.server.daemon_threads = True
        self.thread = None

    @property
    def port(self):
        return self.server.server_address[1]

    @property
    def url(self):
        return "http://%s:%d" % (self.config.get("listen_host") or "127.0.0.1",
                                 self.port)

    def start(self):
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.02})
        self.thread.daemon = True
        self.thread.start()
        return self

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        if self.thread is not None:
            self.thread.join(timeout=5)

    def snapshot(self):
        return self.usage.snapshot(self.config)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def http_json(url, payload=None, timeout=10.0):
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=headers)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8", "replace"))


def build_parser():
    parser = argparse.ArgumentParser(
        description="transparent AI gateway with usage metering and fault injection")
    parser.add_argument("--config", default=None, help="JSON config file")
    parser.add_argument("--upstream", default=None,
                        help="upstream base url, e.g. http://127.0.0.1:19120")
    parser.add_argument("--port", type=int, default=None, help="listen port")
    parser.add_argument("--host", default=None, help="listen host")
    parser.add_argument("--delay-ms", type=int, default=None,
                        help="fixed delay added to every request")
    parser.add_argument("--fail-status", type=int, default=None,
                        help="HTTP status to inject (429, 500, ...)")
    parser.add_argument("--fail-times", type=int, default=None,
                        help="how many requests to fail")
    parser.add_argument("--reset-times", type=int, default=None,
                        help="how many connections to reset")
    parser.add_argument("--loss-times", type=int, default=None,
                        help="how many responses to swallow after a successful call")
    parser.add_argument("--print-usage", action="store_true",
                        help="client: print GET /__usage of a running gateway")
    parser.add_argument("--reset-usage", action="store_true",
                        help="client: POST /__usage/reset of a running gateway")
    return parser


def cli_overrides(args):
    overrides = {}
    if args.upstream:
        overrides["upstream"] = args.upstream
    if args.port is not None:
        overrides["listen_port"] = args.port
    if args.host:
        overrides["listen_host"] = args.host
    if args.delay_ms is not None:
        overrides["delay_ms"] = args.delay_ms
    if args.fail_status is not None:
        overrides["fail_status"] = args.fail_status
    if args.fail_times is not None:
        overrides["fail_times"] = args.fail_times
    if args.reset_times is not None:
        overrides["reset_times"] = args.reset_times
    if args.loss_times is not None:
        overrides["loss_times"] = args.loss_times
    return overrides


def client_port(args):
    """Port of an already running gateway, for --print-usage/--reset-usage."""
    if args.port is not None:
        return int(args.port)
    if args.config:
        with open(args.config, "r") as handle:
            file_config = json.load(handle)
        return int(file_config.get("listen_port",
                                   DEFAULT_CONFIG["listen_port"]))
    return int(os.environ.get("AI_GATEWAY_PORT",
                              DEFAULT_CONFIG["listen_port"]))


def main(argv):
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.print_usage:
        try:
            usage = http_json("http://127.0.0.1:%d/__usage" % client_port(args))
        except (urllib.error.URLError, socket.error, OSError):
            print("GATEWAY NOT RUNNING")
            return 1
        print(json.dumps(usage, indent=2, sort_keys=True))
        return 0
    if args.reset_usage:
        try:
            http_json("http://127.0.0.1:%d/__usage/reset" % client_port(args),
                      payload={})
        except (urllib.error.URLError, socket.error, OSError):
            print("GATEWAY NOT RUNNING")
            return 1
        print("USAGE RESET")
        return 0

    config = load_config(args.config, cli_overrides(args))
    gateway = Gateway(config)
    print("LISTENING %d -> %s" % (gateway.port, config["upstream"]))
    sys.stdout.flush()
    try:
        gateway.server.serve_forever(poll_interval=0.05)
    except KeyboardInterrupt:
        pass
    finally:
        gateway.stop()
        print("USAGE " + json.dumps(gateway.snapshot()["totals"], sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
