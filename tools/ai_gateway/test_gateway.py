#!/usr/bin/env python3
# -*- coding: ascii -*-
"""Unit tests for tools/ai_gateway/gateway.py.

Everything runs on 127.0.0.1 with a fake upstream, so no model provider and no
credentials are involved.  Run with:

    python3 -m unittest discover -s tools/ai_gateway -p 'test_*.py' -v
    python3 tools/ai_gateway/test_gateway.py -v
"""

import http.client
import json
import os
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gateway  # noqa: E402  (path setup must run first)


REPLY_TEXT = "fixed answer"          # 12 characters -> 3 output tokens
LONG_PROMPT = "x" * 400              # 400 characters -> 100 input tokens


class Closed(Exception):
    """The peer closed the connection without a response."""


def make_upstream(reply_text=REPLY_TEXT, delay_ms=0):
    """Return (server, port, calls) for a fake OpenAI style upstream."""
    calls = []

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass

        def do_POST(self):
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length) if length else b""
            calls.append({
                "path": self.path,
                "headers": {key.lower(): value
                            for key, value in self.headers.items()},
                "body": body,
            })
            if delay_ms:
                time.sleep(delay_ms / 1000.0)
            try:
                payload = json.loads(body.decode("utf-8"))
            except ValueError:
                payload = {}
            reply = {
                "id": "fake",
                "model": payload.get("model", ""),
                "choices": [
                    {"index": 0,
                     "message": {"role": "assistant", "content": reply_text},
                     "finish_reason": "stop"}
                ],
                "usage": {"prompt_tokens": 1, "completion_tokens": 1,
                          "total_tokens": 2},
            }
            raw = json.dumps(reply).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
            self.wfile.flush()

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever,
                              kwargs={"poll_interval": 0.02})
    thread.daemon = True
    thread.start()
    return server, server.server_address[1], calls


def post(port, path, payload, timeout=10):
    """POST JSON; raise Closed when the peer drops the connection."""
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    body = json.dumps(payload)
    try:
        connection.request("POST", path, body=body,
                           headers={"Content-Type": "application/json",
                                    "Authorization": "Bearer sk-test"})
        response = connection.getresponse()
        return response.status, response.read().decode("utf-8", "replace")
    except (http.client.RemoteDisconnected, http.client.IncompleteRead,
            http.client.BadStatusLine, ConnectionResetError, ConnectionError,
            BrokenPipeError) as exc:
        raise Closed(str(exc))
    finally:
        connection.close()


def get_json(port, path, timeout=10):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        connection.request("GET", path)
        response = connection.getresponse()
        return response.status, json.loads(response.read().decode("utf-8"))
    finally:
        connection.close()


def chat_request(model="m1", text=LONG_PROMPT):
    return {"model": model, "messages": [{"role": "user", "content": text}]}


class GatewayTest(unittest.TestCase):
    """Base class: one fake upstream and any number of gateways per test."""

    def setUp(self):
        self.upstream_server, self.upstream_port, self.upstream_calls = \
            make_upstream()
        self.gateways = []

    def tearDown(self):
        for running in self.gateways:
            running.stop()
        self.upstream_server.shutdown()
        self.upstream_server.server_close()

    def start_gateway(self, **overrides):
        overrides.setdefault(
            "upstream", "http://127.0.0.1:%d" % self.upstream_port)
        overrides.setdefault("listen_port", 0)
        config = gateway.load_config(None, overrides)
        running = gateway.Gateway(config).start()
        self.gateways.append(running)
        return running

    def usage(self, running):
        status, payload = get_json(running.port, "/__usage")
        self.assertEqual(status, 200)
        return payload


class TestPassThrough(GatewayTest):

    def test_response_and_headers_are_forwarded(self):
        running = self.start_gateway()
        status, body = post(running.port, "/v1/chat/completions", chat_request())
        self.assertEqual(200, status)
        payload = json.loads(body)
        self.assertEqual(REPLY_TEXT, payload["choices"][0]["message"]["content"])
        self.assertEqual(1, len(self.upstream_calls))
        self.assertEqual("/v1/chat/completions", self.upstream_calls[0]["path"])
        self.assertEqual("Bearer sk-test",
                         self.upstream_calls[0]["headers"].get("authorization"))
        self.assertEqual(json.loads(self.upstream_calls[0]["body"])["model"], "m1")

    def test_embeddings_pass_through(self):
        running = self.start_gateway()
        status, _body = post(running.port, "/v1/embeddings",
                             {"model": "e1", "input": ["a", "b"]})
        self.assertEqual(200, status)
        self.assertEqual("/v1/embeddings", self.upstream_calls[0]["path"])

    def test_health_endpoint(self):
        running = self.start_gateway()
        status, payload = get_json(running.port, "/__health")
        self.assertEqual(200, status)
        self.assertTrue(payload["ok"])


class TestUsageCounters(GatewayTest):

    def test_per_model_counters(self):
        running = self.start_gateway()
        post(running.port, "/v1/chat/completions", chat_request("m1", "a"))
        post(running.port, "/v1/chat/completions", chat_request("m1", "b"))
        post(running.port, "/v1/embeddings", {"model": "m2", "input": ["c"]})
        usage = self.usage(running)
        self.assertEqual(3, usage["totals"]["requests"])
        self.assertEqual(3, usage["totals"]["upstream_calls"])
        self.assertEqual(2, usage["by_model"]["m1"]["requests"])
        self.assertEqual(1, usage["by_model"]["m2"]["requests"])
        self.assertEqual(2, usage["by_model"]["m1"]["upstream_calls"])
        self.assertEqual("USD", usage["currency"])

    def test_input_token_heuristic_is_chars_over_four(self):
        running = self.start_gateway()
        post(running.port, "/v1/chat/completions", chat_request("m1", LONG_PROMPT))
        usage = self.usage(running)
        self.assertEqual(100, usage["totals"]["input_tokens"])
        self.assertEqual(3, usage["totals"]["output_tokens"])
        self.assertEqual(103, usage["totals"]["total_tokens"])
        self.assertEqual(100, usage["by_model"]["m1"]["input_tokens"])

    def test_estimate_tokens_helper(self):
        self.assertEqual(0, gateway.estimate_tokens(""))
        self.assertEqual(1, gateway.estimate_tokens("ab"))
        self.assertEqual(1, gateway.estimate_tokens("abcd"))
        self.assertEqual(2, gateway.estimate_tokens("abcde"))

    def test_latency_is_recorded(self):
        self.upstream_server.shutdown()
        self.upstream_server.server_close()
        self.upstream_server, self.upstream_port, self.upstream_calls = \
            make_upstream(delay_ms=60)
        running = self.start_gateway()
        post(running.port, "/v1/chat/completions", chat_request())
        usage = self.usage(running)
        self.assertGreaterEqual(usage["latency_ms"]["max"], 50)
        self.assertGreaterEqual(usage["by_model"]["m1"]["latency_ms_avg"], 50)

    def test_reset_usage(self):
        running = self.start_gateway()
        post(running.port, "/v1/chat/completions", chat_request())
        self.assertEqual(1, self.usage(running)["totals"]["requests"])
        status, body = post(running.port, "/__usage/reset", {})
        self.assertEqual(200, status)
        self.assertTrue(json.loads(body)["reset"])
        self.assertEqual(0, self.usage(running)["totals"]["requests"])


class TestFaultInjection(GatewayTest):

    def test_injected_429_does_not_reach_upstream(self):
        running = self.start_gateway(fail_status=429, fail_times=1)
        status, body = post(running.port, "/v1/chat/completions",
                            chat_request("m1", "first"))
        self.assertEqual(429, status)
        self.assertEqual("gateway_injected", json.loads(body)["error"]["type"])
        self.assertEqual([], self.upstream_calls)
        status, _body = post(running.port, "/v1/chat/completions",
                             chat_request("m1", "second"))
        self.assertEqual(200, status)
        self.assertEqual(1, len(self.upstream_calls))
        usage = self.usage(running)
        self.assertEqual(2, usage["totals"]["requests"])
        self.assertEqual(1, usage["totals"]["injected_failures"])
        self.assertEqual(1, usage["totals"]["upstream_calls"])

    def test_injected_500_and_retries_are_counted(self):
        running = self.start_gateway(fail_status=500, fail_times=2)
        request = chat_request("m1", "same prompt")
        self.assertEqual(500, post(running.port, "/v1/chat/completions", request)[0])
        self.assertEqual(500, post(running.port, "/v1/chat/completions", request)[0])
        self.assertEqual(200, post(running.port, "/v1/chat/completions", request)[0])
        usage = self.usage(running)
        self.assertEqual(3, usage["totals"]["requests"])
        self.assertEqual(2, usage["totals"]["injected_failures"])
        self.assertEqual(1, usage["totals"]["upstream_calls"])
        # the 2nd and 3rd identical request are retries of the 1st one
        self.assertEqual(2, usage["totals"]["retries"])
        self.assertEqual(2, usage["by_model"]["m1"]["retries"])

    def test_connection_reset(self):
        running = self.start_gateway(reset_times=1)
        with self.assertRaises(Closed):
            post(running.port, "/v1/chat/completions", chat_request("m1", "reset"))
        self.assertEqual([], self.upstream_calls)
        usage = self.usage(running)
        self.assertEqual(1, usage["totals"]["injected_resets"])
        self.assertEqual(0, usage["totals"]["upstream_calls"])
        self.assertEqual(0, usage["totals"]["input_tokens"])
        status, _body = post(running.port, "/v1/chat/completions",
                             chat_request("m1", "after reset"))
        self.assertEqual(200, status)

    def test_response_loss_still_counts_the_upstream_call(self):
        running = self.start_gateway(loss_times=1)
        with self.assertRaises(Closed):
            post(running.port, "/v1/chat/completions", chat_request("m1", LONG_PROMPT))
        self.assertEqual(1, len(self.upstream_calls))
        usage = self.usage(running)
        self.assertEqual(1, usage["totals"]["lost_responses"])
        self.assertEqual(1, usage["totals"]["upstream_calls"])
        self.assertEqual(1, usage["totals"]["requests"])
        # the upstream answered, so the tokens are billed even though the
        # database never saw the response
        self.assertEqual(100, usage["totals"]["input_tokens"])
        self.assertEqual(3, usage["totals"]["output_tokens"])

    def test_delay_is_applied(self):
        running = self.start_gateway(delay_ms=120)
        started = time.time()
        status, _body = post(running.port, "/v1/chat/completions", chat_request())
        elapsed_ms = (time.time() - started) * 1000.0
        self.assertEqual(200, status)
        self.assertGreaterEqual(elapsed_ms, 110)

    def test_delay_distribution_is_applied(self):
        running = self.start_gateway(
            delay_distribution={"type": "uniform", "min_ms": 40, "max_ms": 60})
        started = time.time()
        status, _body = post(running.port, "/v1/chat/completions", chat_request())
        elapsed_ms = (time.time() - started) * 1000.0
        self.assertEqual(200, status)
        self.assertGreaterEqual(elapsed_ms, 35)

    def test_fail_forever(self):
        running = self.start_gateway(fail_status=503, fail_forever=True)
        for _index in range(3):
            self.assertEqual(503, post(running.port, "/v1/chat/completions",
                                       chat_request())[0])
        self.assertEqual([], self.upstream_calls)
        self.assertEqual(3, self.usage(running)["totals"]["injected_failures"])


class TestCostEstimation(GatewayTest):

    def test_flat_prices(self):
        running = self.start_gateway(prices={
            "currency": "USD",
            "per_1k_input": 0.001,
            "per_1k_output": 0.002,
        })
        post(running.port, "/v1/chat/completions", chat_request("m1", LONG_PROMPT))
        usage = self.usage(running)
        expected = (100 / 1000.0) * 0.001 + (3 / 1000.0) * 0.002
        self.assertAlmostEqual(expected, usage["totals"]["estimated_cost"],
                               places=12)
        self.assertAlmostEqual(expected, usage["by_model"]["m1"]["estimated_cost"],
                               places=12)

    def test_per_model_prices_override_the_flat_ones(self):
        running = self.start_gateway(prices={
            "currency": "CNY",
            "per_1k_input": 0.001,
            "per_1k_output": 0.002,
            "models": {"m2": {"per_1k_input": 0.010, "per_1k_output": 0.020}},
        })
        post(running.port, "/v1/chat/completions", chat_request("m1", LONG_PROMPT))
        post(running.port, "/v1/chat/completions", chat_request("m2", LONG_PROMPT))
        usage = self.usage(running)
        cheap = (100 / 1000.0) * 0.001 + (3 / 1000.0) * 0.002
        pricey = (100 / 1000.0) * 0.010 + (3 / 1000.0) * 0.020
        self.assertAlmostEqual(cheap, usage["by_model"]["m1"]["estimated_cost"],
                               places=12)
        self.assertAlmostEqual(pricey, usage["by_model"]["m2"]["estimated_cost"],
                               places=12)
        self.assertAlmostEqual(cheap + pricey, usage["totals"]["estimated_cost"],
                               places=12)
        self.assertEqual("CNY", usage["currency"])

    def test_no_prices_means_zero_cost(self):
        running = self.start_gateway()
        post(running.port, "/v1/chat/completions", chat_request())
        self.assertEqual(0.0, self.usage(running)["totals"]["estimated_cost"])


class TestConfiguration(unittest.TestCase):

    def test_upstream_is_required(self):
        with self.assertRaises(ValueError):
            gateway.load_config(None, {})

    def test_bad_fail_status_is_rejected(self):
        with self.assertRaises(ValueError):
            gateway.load_config(None, {"upstream": "http://127.0.0.1:1",
                                       "fail_status": 42})

    def test_cli_overrides(self):
        config = gateway.load_config(None, {"upstream": "http://x:1",
                                            "listen_port": 20000})
        self.assertEqual(20000, config["listen_port"])
        self.assertEqual(30000, config["timeout_ms"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
