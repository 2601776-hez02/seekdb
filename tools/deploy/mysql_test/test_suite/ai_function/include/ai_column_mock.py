#!/usr/bin/env python3
# -*- coding: ascii -*-
"""Deterministic mock AI service for the AI COLUMN mysqltest suite.

The mock speaks the OpenAI wire format (plus the Ollama one) so it can be wired
into ``DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT`` exactly like a real provider:

    url = http://127.0.0.1:19120/v1/chat/completions   (provider=openai)
    url = http://127.0.0.1:19120/v1/embeddings         (provider=openai)
    url = http://127.0.0.1:19120/api/chat              (provider=ollama)
    url = http://127.0.0.1:19120/api/embed             (provider=ollama)

Nothing in this file talks to the network except 127.0.0.1, and everything is
standard library only (http.server, json, threading, hashlib, argparse, time,
os, signal, socket, sys, tempfile, urllib).

Response determinism
--------------------
The reply is a pure function of the request text, so a test can predict it:

  [[OUT=LANG]]   completion returns a language name (hash of the prompt, or the
                 value of a [[LANG=xxx]] marker found in the prompt)
  [[OUT=TAGS]]   completion returns a JSON array of tags (hash of the prompt, or
                 the comma separated value of a [[TAGS=a,b]] marker)
  [[OUT=ECHO]]   completion returns the value of a [[ECHO=xxx]] marker, or
                 ``echo-<hash8>`` when the marker is absent
  no marker      a [[ECHO=]] / [[LANG=]] / [[TAGS=]] marker if present, else a
                 language name derived from the stable hash of the prompt

Embeddings: a unit-length vector of ``dim`` floats derived from the stable hash
of the input, where ``dim`` is the request ``dimensions`` field, else a
``#dim=N`` marker inside the prompt, else ``--dim``.  A ``[[VEC=a,b,c,d]]``
marker returns exactly those numbers instead, so a test can hand write the
expected vector and the expected ANN order.

Fault injection markers (inside the prompt/input text, anywhere):

  #fail          answer HTTP 500
  #http=CODE     answer HTTP CODE (e.g. #http=429, #http=503)
  #bad           answer HTTP 200 with a truncated, non-JSON body
  #slow=MS       sleep MS milliseconds before answering
  #dim=N         force the embedding dimension to N

Control plane (used by the .test files):

  GET  /__mock/state                 JSON: counters, config, last N requests
  GET  /__mock/state?format=text     one stable line, see state_text() below
  GET  /__mock/state?contains=TEXT   additionally counts requests whose prompt
                                     contains TEXT (case insensitive)
  POST /__mock/config  {...}         patch delay_ms / fail_times / fail_status
                                     / fail_forever / drop_response_times
                                     / max_concurrency
  POST /__mock/reset                 zero the counters, restore the defaults
  POST /__mock/shutdown              stop the server the same way SIGTERM does
  GET  /__mock/ping                  liveness probe used by --wait-ready

Faults are consumed in this order for every request: config fail_forever /
fail_times -> marker (#http / #fail / #bad) -> drop_response_times.

CLI
---
Server mode (default):

  python3 ai_column_mock.py [--port N] [--model-name NAME] [--dim N] [--seed N]

  The port defaults to $AI_COLUMN_MOCK_PORT, else 19120.  ``--dim`` defaults to
  $AI_COLUMN_MOCK_DIM, else 4.  Ready is signalled by ``LISTENING <port>`` on
  stdout.  SIGTERM / SIGINT shut the server down cleanly.

Client mode (drive a server that is already running on the same port):

  --wait-ready            poll until the server answers, print MOCK READY
  --reset                 POST /__mock/reset, print MOCK RESET
  --config JSON           POST /__mock/config, print MOCK CONFIG OK
  --print-state           print the one line state, honours --contains/--state-limit
  --assert-contains TEXT  print CONTAINS PRESENT and exit 0 when a recorded
                          prompt contains TEXT, else print CONTAINS ABSENT and
                          exit 1 (a real assertion for the .test files)
  --stop / --stop-if-running
                          stop the server on that port (POST /__mock/shutdown,
                          then SIGTERM on the pid file as a fallback) and print
                          MOCK STOPPED (never fails when nothing runs)

Exit code is 0 on success and 1 on a refused/failed control request, so a
mysqltest ``--exec`` line aborts the case when the mock is not there.
"""

import argparse
import hashlib
import json
import os
import signal
import socket
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# --------------------------------------------------------------------------
# deterministic tables
# --------------------------------------------------------------------------

LANGUAGES = [
    "Chinese",
    "English",
    "French",
    "German",
    "Japanese",
    "Spanish",
    "Portuguese",
    "Russian",
]

TAG_WORDS = [
    "news",
    "tech",
    "sports",
    "music",
    "finance",
    "travel",
    "science",
    "health",
]

DEFAULT_PORT = 19120
DEFAULT_DIM = 4
DEFAULT_SEED = 0
DEFAULT_MODEL_NAME = "mock-model"

DEFAULT_CONFIG = {
    "delay_ms": 0,
    "fail_times": 0,
    "fail_status": 500,
    "fail_forever": False,
    "drop_response_times": 0,
    "max_concurrency": 0,
}

MAX_RECORDED_PROMPT = 65536
DEFAULT_STATE_LIMIT = 20


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------


def stable_digest(text, seed):
    """Return the hex sha256 of (seed, text); the single source of determinism."""
    digest = hashlib.sha256()
    digest.update(str(seed).encode("ascii", "replace"))
    digest.update(b"\x00")
    digest.update(text.encode("utf-8", "replace"))
    return digest.hexdigest()


def digest_int(text, seed):
    """Return a positive integer derived from the stable digest."""
    return int(stable_digest(text, seed)[:12], 16)


def make_vector(text, seed, dim):
    """Return a unit-length, dim dimensional vector derived from `text`."""
    if dim <= 0:
        dim = 1
    if dim > 4096:
        dim = 4096
    values = []
    counter = 0
    while len(values) < dim:
        piece = stable_digest("%s#%d" % (text, counter), seed)
        for offset in range(0, len(piece) - 8, 8):
            raw = int(piece[offset:offset + 8], 16)
            values.append((raw / 4294967295.0) * 2.0 - 1.0)
            if len(values) >= dim:
                break
        counter += 1
    norm = sum(value * value for value in values) ** 0.5
    if norm == 0.0:
        values[0] = 1.0
        norm = 1.0
    return [round(value / norm, 6) for value in values]


def marker_value(text, name):
    """Return the value of ``[[NAME=value]]`` or None."""
    if not text:
        return None
    opening = "[[" + name + "="
    start = text.find(opening)
    if start < 0:
        return None
    start += len(opening)
    end = text.find("]]", start)
    if end < 0:
        return None
    return text[start:end]


def marker_int(text, name):
    """Return the integer value of ``#name=N`` or None."""
    if not text:
        return None
    opening = "#" + name + "="
    start = text.find(opening)
    if start < 0:
        return None
    start += len(opening)
    end = start
    while end < len(text) and (text[end].isdigit() or text[end] in "+-"):
        end += 1
    if end == start:
        return None
    try:
        return int(text[start:end])
    except ValueError:
        return None


def default_tags(text, seed):
    """Return two deterministic tag words for `text`."""
    base = digest_int(text, seed)
    first = TAG_WORDS[base % len(TAG_WORDS)]
    second = TAG_WORDS[(base // len(TAG_WORDS)) % len(TAG_WORDS)]
    if second == first:
        second = TAG_WORDS[(base // (len(TAG_WORDS) * len(TAG_WORDS))) % len(TAG_WORDS)]
    if second == first:
        second = TAG_WORDS[(TAG_WORDS.index(first) + 1) % len(TAG_WORDS)]
    return [first, second]


def tags_payload(value):
    """Turn ``a,b`` into a JSON array string."""
    items = [item.strip() for item in value.split(",") if item.strip()]
    return json.dumps(items)


def parse_vector(value):
    """Turn ``1,0,0.5`` into [1.0, 0.0, 0.5], or None when it is not numeric."""
    values = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            return None
        try:
            values.append(float(item))
        except ValueError:
            return None
    return values or None


def completion_text(prompt, seed):
    """Return the deterministic completion text for `prompt`."""
    out = marker_value(prompt, "OUT")
    out = out.strip().upper() if out is not None else None
    echo = marker_value(prompt, "ECHO")
    lang = marker_value(prompt, "LANG")
    tags = marker_value(prompt, "TAGS")

    if out == "ECHO":
        if echo is not None:
            return echo
        return "echo-" + stable_digest(prompt, seed)[:8]
    if out == "TAGS":
        if tags is not None:
            return tags_payload(tags)
        return json.dumps(default_tags(prompt, seed))
    if out == "LANG":
        if lang is not None:
            return lang.strip()
        return LANGUAGES[digest_int(prompt, seed) % len(LANGUAGES)]

    # no [[OUT=]] marker: keep the plain marker behaviour
    if echo is not None:
        return echo
    if lang is not None:
        return lang.strip()
    if tags is not None:
        return tags_payload(tags)
    return LANGUAGES[digest_int(prompt, seed) % len(LANGUAGES)]


def count_chars(text):
    return len(text) if text else 0


# --------------------------------------------------------------------------
# mock state
# --------------------------------------------------------------------------


class MockState(object):
    """Counters, config and the append-only request log."""

    def __init__(self, seed, model_name, dim, port):
        self.lock = threading.Lock()
        self.seed = seed
        self.model_name = model_name
        self.dim = dim
        self.port = port
        self.config = dict(DEFAULT_CONFIG)
        self.requests = []
        self.total = 0
        self.by_path = {}
        self.by_model = {}
        self.by_outcome = {}
        self.failed = 0
        self.dropped = 0
        self.throttled = 0
        self.concurrency_now = 0
        self.observed_max_concurrency = 0

    def reset(self):
        with self.lock:
            self.config = dict(DEFAULT_CONFIG)
            self.requests = []
            self.total = 0
            self.by_path = {}
            self.by_model = {}
            self.by_outcome = {}
            self.failed = 0
            self.dropped = 0
            self.throttled = 0
            self.concurrency_now = 0
            self.observed_max_concurrency = 0

    def patch_config(self, patch):
        if not isinstance(patch, dict):
            raise ValueError("config body must be a JSON object")
        with self.lock:
            for key in patch:
                if key not in DEFAULT_CONFIG:
                    raise ValueError("unknown config key '%s'" % key)
            updated = dict(self.config)
            for key in patch:
                value = patch[key]
                default = DEFAULT_CONFIG[key]
                if isinstance(default, bool):
                    updated[key] = bool(value)
                else:
                    updated[key] = int(value)
            if updated["fail_status"] < 100 or updated["fail_status"] > 599:
                raise ValueError("fail_status must be in [100, 599]")
            for key in ("delay_ms", "fail_times", "drop_response_times",
                        "max_concurrency"):
                if updated[key] < 0:
                    raise ValueError("%s must be >= 0" % key)
            self.config = updated

    def enter(self):
        with self.lock:
            self.concurrency_now += 1
            if self.concurrency_now > self.observed_max_concurrency:
                self.observed_max_concurrency = self.concurrency_now
            return self.concurrency_now

    def leave(self):
        with self.lock:
            if self.concurrency_now > 0:
                self.concurrency_now -= 1

    def take_failure(self):
        """Consume one injected config failure, return True when it fired."""
        with self.lock:
            if self.config["fail_forever"]:
                return True
            if self.config["fail_times"] > 0:
                self.config["fail_times"] -= 1
                return True
            return False

    def take_drop(self):
        """Consume one response drop, return True when it fired."""
        with self.lock:
            if self.config["drop_response_times"] > 0:
                self.config["drop_response_times"] -= 1
                return True
            return False

    def record(self, path, model, prompt, dim, headers, status, outcome,
               elapsed_ms):
        with self.lock:
            entry = {
                "seq": self.total + 1,
                "path": path,
                "model": model,
                "prompt": (prompt or "")[:MAX_RECORDED_PROMPT],
                "prompt_chars": count_chars(prompt),
                "dim": dim,
                "headers": headers,
                "status": status,
                "outcome": outcome,
                "elapsed_ms": elapsed_ms,
            }
            self.requests.append(entry)
            self.total += 1
            self.by_path[path] = self.by_path.get(path, 0) + 1
            key = model if model else "(none)"
            self.by_model[key] = self.by_model.get(key, 0) + 1
            self.by_outcome[outcome] = self.by_outcome.get(outcome, 0) + 1
            if outcome == "failed":
                self.failed += 1
            elif outcome == "dropped":
                self.dropped += 1
            elif outcome == "throttled":
                self.throttled += 1

    def count_contains(self, needle):
        if not needle:
            return 0
        lowered = needle.lower()
        with self.lock:
            return sum(1 for entry in self.requests
                       if lowered in entry["prompt"].lower())

    def state_dict(self, limit, contains):
        with self.lock:
            last = self.requests[-limit:] if limit > 0 else []
            result = {
                "ok": True,
                "server": {
                    "port": self.port,
                    "model_name": self.model_name,
                    "dim": self.dim,
                    "seed": self.seed,
                },
                "config": dict(self.config),
                "counters": {
                    "total": self.total,
                    "by_path": dict(self.by_path),
                    "by_model": dict(self.by_model),
                    "by_outcome": dict(self.by_outcome),
                    "failed": self.failed,
                    "dropped": self.dropped,
                    "throttled": self.throttled,
                    "concurrency_now": self.concurrency_now,
                    "observed_max_concurrency": self.observed_max_concurrency,
                },
                "requests": last,
            }
        if contains is not None:
            result["contains"] = {
                "needle": contains,
                "count": self.count_contains(contains),
            }
        return result

    def state_text(self, contains):
        """One stable line, safe to compare from a mysqltest result file."""
        with self.lock:
            snapshot = {
                "total": self.total,
                "complete": self.by_path.get("/v1/chat/completions", 0),
                "embed": self.by_path.get("/v1/embeddings", 0),
                "ollama_chat": self.by_path.get("/api/chat", 0),
                "ollama_embed": self.by_path.get("/api/embed", 0),
                "failed": self.failed,
                "dropped": self.dropped,
                "throttled": self.throttled,
                "peak": self.observed_max_concurrency,
                "models": dict(self.by_model),
            }
        order = ["total", "complete", "embed", "ollama_chat", "ollama_embed",
                 "failed", "dropped", "throttled", "peak"]
        text = " ".join("%s=%d" % (name, snapshot[name]) for name in order)
        models = snapshot["models"]
        rendered = ",".join("%s:%d" % (name, models[name])
                            for name in sorted(models)) or "-"
        text += " models=" + rendered
        if contains is not None:
            text += " contains=%d" % self.count_contains(contains)
        return text


# --------------------------------------------------------------------------
# request parsing
# --------------------------------------------------------------------------


def parse_json_body(raw):
    if not raw:
        return {}
    try:
        parsed = json.loads(raw.decode("utf-8", "replace"))
    except ValueError:
        return {}
    return parsed if isinstance(parsed, dict) else {}


def message_text(body):
    """Return the concatenated content of the ``messages`` array."""
    messages = body.get("messages")
    if not isinstance(messages, list):
        return ""
    parts = []
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
    return "\n".join(parts)


def input_text(body):
    """Return the concatenated ``input`` of an embeddings request."""
    value = body.get("input")
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        parts = []
        for item in value:
            if isinstance(item, str):
                parts.append(item)
            elif isinstance(item, dict) and isinstance(item.get("text"), str):
                parts.append(item["text"])
        return "\n".join(parts)
    return ""


def extract_request(path, body):
    """Return (model, prompt, dim) for any supported path."""
    model = body.get("model")
    if not isinstance(model, str) or not model:
        model = ""
    if path.endswith("/embeddings") or path.endswith("/api/embed"):
        prompt = input_text(body)
    elif path.endswith("/chat/completions") or path.endswith("/api/chat"):
        prompt = message_text(body)
    else:
        prompt = input_text(body) or message_text(body)
    dim = body.get("dimensions")
    try:
        dim = int(dim) if dim is not None else 0
    except (TypeError, ValueError):
        dim = 0
    return model, prompt, dim


def path_kind(path):
    if path.endswith("/v1/chat/completions"):
        return "chat"
    if path.endswith("/v1/embeddings"):
        return "embed"
    if path.endswith("/api/chat"):
        return "ollama_chat"
    if path.endswith("/api/embed"):
        return "ollama_embed"
    return None


# --------------------------------------------------------------------------
# responses
# --------------------------------------------------------------------------


def error_body(status, message):
    return {
        "error": {
            "message": message,
            "type": "mock_error",
            "code": status,
        }
    }


def chat_response(model, text, prompt):
    prompt_tokens = max(1, count_chars(prompt) // 4)
    completion_tokens = max(1, count_chars(text) // 4)
    return {
        "id": "chatcmpl-mock",
        "object": "chat.completion",
        "created": int(time.time()),
        "model": model,
        "choices": [
            {
                "index": 0,
                "message": {"role": "assistant", "content": text},
                "finish_reason": "stop",
            }
        ],
        "usage": {
            "prompt_tokens": prompt_tokens,
            "completion_tokens": completion_tokens,
            "total_tokens": prompt_tokens + completion_tokens,
        },
    }


def embed_response(model, vectors, prompt):
    prompt_tokens = max(1, count_chars(prompt) // 4)
    return {
        "object": "list",
        "model": model,
        "data": [
            {"object": "embedding", "index": index, "embedding": vector}
            for index, vector in enumerate(vectors)
        ],
        "usage": {"prompt_tokens": prompt_tokens, "total_tokens": prompt_tokens},
    }


def ollama_chat_response(model, text):
    return {
        "model": model,
        "created_at": "2025-01-01T00:00:00Z",
        "message": {"role": "assistant", "content": text},
        "done": True,
    }


def ollama_embed_response(model, vectors):
    return {"model": model, "embeddings": vectors, "done": True}


# --------------------------------------------------------------------------
# HTTP handling
# --------------------------------------------------------------------------


class MockHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "ai-column-mock/1.0"
    mock_state = None      # filled by build_handler()
    mock_shutdown = None   # filled by build_handler()

    def log_message(self, fmt, *args):  # keep the test log clean
        if os.environ.get("AI_COLUMN_MOCK_VERBOSE"):
            sys.stderr.write("mock: " + (fmt % args) + "\n")

    # -- plumbing ---------------------------------------------------------

    def read_body(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        if length <= 0:
            return b""
        return self.rfile.read(length)

    def header_map(self):
        result = {}
        for name in self.headers.keys():
            result[name.lower()] = self.headers.get(name)
        return result

    def send_json(self, status, payload):
        raw = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)
        self.wfile.flush()

    def send_raw(self, status, raw, content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)
        self.wfile.flush()

    def drop_connection(self):
        """Read the request, then close without writing any response."""
        self.close_connection = True
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.connection.close()
        except OSError:
            pass

    def query(self):
        parsed = urllib.parse.urlparse(self.path)
        return parsed.path, urllib.parse.parse_qs(parsed.query)

    # -- control plane ----------------------------------------------------

    def do_GET(self):
        path, query = self.query()
        state = self.mock_state
        if path == "/__mock/state":
            limit = DEFAULT_STATE_LIMIT
            if "limit" in query:
                try:
                    limit = int(query["limit"][0])
                except (TypeError, ValueError):
                    limit = DEFAULT_STATE_LIMIT
            contains = query.get("contains", [None])[0]
            fmt = query.get("format", ["json"])[0]
            if fmt == "text":
                text = state.state_text(contains)
                self.send_raw(200, (text + "\n").encode("utf-8"), "text/plain")
            else:
                self.send_json(200, state.state_dict(limit, contains))
        elif path == "/__mock/ping":
            self.send_json(200, {"ok": True})
        else:
            self.send_json(404, error_body(404, "not found: " + path))

    def do_POST(self):
        path, _query = self.query()
        body_raw = self.read_body()

        if path == "/__mock/config":
            self.handle_config(body_raw)
            return
        if path == "/__mock/reset":
            self.mock_state.reset()
            self.send_json(200, {"ok": True, "reset": True})
            return
        if path == "/__mock/shutdown":
            self.send_json(200, {"ok": True, "shutdown": True})
            threading.Thread(target=self.mock_shutdown).start()
            return
        if path_kind(path) is None:
            self.send_json(404, error_body(404, "not found: " + path))
            return
        self.handle_model_request(path, body_raw)

    def handle_config(self, body_raw):
        try:
            payload = json.loads(body_raw.decode("utf-8", "replace") or "{}")
        except ValueError as exc:
            self.send_json(400, error_body(400, "bad json: %s" % exc))
            return
        try:
            self.mock_state.patch_config(payload)
        except ValueError as exc:
            self.send_json(400, error_body(400, str(exc)))
            return
        self.send_json(200, {"ok": True, "config": self.mock_state.config})

    # -- model wire format ------------------------------------------------

    def handle_model_request(self, path, body_raw):
        state = self.mock_state
        started = time.time()
        body = parse_json_body(body_raw)
        model, prompt, dim = extract_request(path, body)
        if not model:
            model = state.model_name

        kind = path_kind(path)
        concurrency = state.enter()
        outcome = "ok"
        status = 200
        try:
            cfg = dict(state.config)
            if cfg["max_concurrency"] > 0 and concurrency > cfg["max_concurrency"]:
                status = 429
                outcome = "throttled"
                self.finish_request(state, path, model, prompt, dim, status,
                                    outcome, started,
                                    payload=error_body(429, "mock concurrency limit"))
                return

            delay_ms = cfg["delay_ms"] + (marker_int(prompt, "slow") or 0)
            if delay_ms > 0:
                time.sleep(delay_ms / 1000.0)

            http_status = marker_int(prompt, "http")
            wants_fail = "#fail" in prompt or (http_status is not None
                                               and http_status >= 400)
            wants_bad = "#bad" in prompt

            if state.take_failure():
                status = cfg["fail_status"]
                outcome = "failed"
                self.finish_request(state, path, model, prompt, dim, status,
                                    outcome, started,
                                    payload=error_body(status, "mock injected failure"))
                return
            if wants_fail:
                status = http_status if http_status is not None else 500
                if status < 400 or status > 599:
                    status = 500
                outcome = "failed"
                self.finish_request(state, path, model, prompt, dim, status,
                                    outcome, started,
                                    payload=error_body(status, "mock marker failure"))
                return
            if wants_bad:
                raw = b'{"choices": [{"message": {"content": '
                self.finish_request(state, path, model, prompt, dim, 200, "bad",
                                    started, raw=raw)
                return
            if state.take_drop():
                self.finish_request(state, path, model, prompt, dim, 0, "dropped",
                                    started, drop=True)
                return

            payload = build_success_payload(kind, model, prompt, dim, state, body)
            self.finish_request(state, path, model, prompt, dim, 200, outcome,
                                started, payload=payload)
        finally:
            state.leave()

    def finish_request(self, state, path, model, prompt, dim, status, outcome,
                       started, payload=None, raw=None, drop=False):
        elapsed_ms = int((time.time() - started) * 1000)
        effective_dim = dim
        if not effective_dim:
            effective_dim = marker_int(prompt, "dim") or state.dim
        state.record(path, model, prompt, effective_dim, self.header_map(),
                     status, outcome, elapsed_ms)
        if drop:
            self.drop_connection()
            return
        if raw is not None:
            self.send_raw(status, raw)
            return
        self.send_json(status, payload if payload is not None else {})


def input_list(body):
    """Return the embeddings request input as a list of strings."""
    value = body.get("input")
    if isinstance(value, str):
        return [value]
    if isinstance(value, list):
        items = []
        for item in value:
            if isinstance(item, str):
                items.append(item)
            elif isinstance(item, dict) and isinstance(item.get("text"), str):
                items.append(item["text"])
        return items or [""]
    return [""]


def build_success_payload(kind, model, prompt, dim, state, body):
    seed = state.seed
    if kind in ("embed", "ollama_embed"):
        effective_dim = dim or marker_int(prompt, "dim") or state.dim
        vectors = []
        for text in input_list(body):
            override = marker_value(text, "VEC")
            vector = parse_vector(override) if override is not None else None
            vectors.append(vector if vector else
                           make_vector(text, seed, effective_dim))
        if kind == "ollama_embed":
            return ollama_embed_response(model, vectors)
        return embed_response(model, vectors, prompt)
    text = completion_text(prompt, seed)
    if kind == "ollama_chat":
        return ollama_chat_response(model, text)
    return chat_response(model, text, prompt)


def build_handler(state, shutdown):
    class BoundHandler(MockHandler):
        mock_state = state
        mock_shutdown = staticmethod(shutdown)

    return BoundHandler


# --------------------------------------------------------------------------
# process control
# --------------------------------------------------------------------------


def default_port():
    try:
        return int(os.environ.get("AI_COLUMN_MOCK_PORT", DEFAULT_PORT))
    except (TypeError, ValueError):
        return DEFAULT_PORT


def default_dim():
    try:
        return int(os.environ.get("AI_COLUMN_MOCK_DIM", DEFAULT_DIM))
    except (TypeError, ValueError):
        return DEFAULT_DIM


def pid_file(port):
    return os.path.join(tempfile.gettempdir(), "ai_column_mock_%d.pid" % port)


def http_request(port, path, payload=None, timeout=10.0):
    """POST when payload is given, GET otherwise; return (status, text)."""
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path),
                                     data=data, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.getcode(), response.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", "replace")


def port_is_listening(port):
    """True when something accepts TCP connections on 127.0.0.1:port."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(0.3)
    try:
        sock.connect(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        try:
            sock.close()
        except OSError:
            pass


def read_pid(path):
    try:
        with open(path, "r") as handle:
            return int(handle.read().strip())
    except (IOError, OSError, ValueError):
        return None


def stop_running(port):
    """Stop the mock that serves `port`.

    The graceful way is ``POST /__mock/shutdown``; it works on Linux and on
    Windows, where a process of the same user may not be allowed to SIGTERM a
    sibling (OpenProcess is refused under a restricted token).  SIGTERM on the
    pid file is still used as a fallback so that the tool behaves like a normal
    daemon on Linux.
    """
    if port_is_listening(port):
        try:
            http_request(port, "/__mock/shutdown", payload={}, timeout=3.0)
        except (urllib.error.URLError, socket.error, OSError):
            pass
        deadline = time.time() + 5.0
        while time.time() < deadline and port_is_listening(port):
            time.sleep(0.05)

    path = pid_file(port)
    pid = read_pid(path)
    if pid is not None and port_is_listening(port):
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
        deadline = time.time() + 3.0
        while time.time() < deadline and port_is_listening(port):
            time.sleep(0.05)

    try:
        os.remove(path)
    except OSError:
        pass
    print("MOCK STOPPED")
    sys.stdout.flush()
    return 0


def client_main(args):
    port = args.port
    if args.stop or args.stop_if_running:
        return stop_running(port)
    if args.wait_ready:
        deadline = time.time() + args.wait_timeout
        while time.time() < deadline:
            try:
                status, _text = http_request(port, "/__mock/ping", timeout=2.0)
                if status == 200:
                    print("MOCK READY")
                    sys.stdout.flush()
                    return 0
            except (urllib.error.URLError, socket.error, OSError):
                pass
            time.sleep(0.1)
        print("MOCK NOT READY")
        sys.stdout.flush()
        return 1
    if args.reset:
        try:
            status, _text = http_request(port, "/__mock/reset", payload={})
        except (urllib.error.URLError, socket.error, OSError):
            print("MOCK NOT RUNNING")
            sys.stdout.flush()
            return 1
        if status != 200:
            print("MOCK RESET FAILED")
            sys.stdout.flush()
            return 1
        print("MOCK RESET")
        sys.stdout.flush()
        return 0
    if args.config is not None:
        try:
            payload = json.loads(args.config)
        except ValueError as exc:
            sys.stderr.write("bad --config json: %s\n" % exc)
            return 1
        try:
            status, _text = http_request(port, "/__mock/config", payload=payload)
        except (urllib.error.URLError, socket.error, OSError):
            print("MOCK NOT RUNNING")
            sys.stdout.flush()
            return 1
        if status != 200:
            print("MOCK CONFIG FAILED")
            sys.stdout.flush()
            return 1
        print("MOCK CONFIG OK")
        sys.stdout.flush()
        return 0
    if args.assert_contains is not None:
        path = "/__mock/state?format=text&limit=0&contains=" + \
               urllib.parse.quote(args.assert_contains)
        try:
            status, text = http_request(port, path)
        except (urllib.error.URLError, socket.error, OSError):
            print("MOCK NOT RUNNING")
            sys.stdout.flush()
            return 1
        if status != 200:
            print("MOCK STATE FAILED")
            sys.stdout.flush()
            return 1
        match = None
        for field in text.split():
            if field.startswith("contains="):
                match = field
                break
        count = int(match.split("=", 1)[1]) if match else 0
        if count > 0:
            print("CONTAINS PRESENT")
            sys.stdout.flush()
            return 0
        print("CONTAINS ABSENT")
        sys.stdout.flush()
        return 1
    if args.print_state:
        path = "/__mock/state?format=text&limit=%d" % args.state_limit
        if args.contains:
            path += "&contains=" + urllib.parse.quote(args.contains)
        try:
            status, text = http_request(port, path)
        except (urllib.error.URLError, socket.error, OSError):
            print("MOCK NOT RUNNING")
            sys.stdout.flush()
            return 1
        if status != 200:
            print("MOCK STATE FAILED")
            sys.stdout.flush()
            return 1
        sys.stdout.write(text if text.endswith("\n") else text + "\n")
        sys.stdout.flush()
        return 0
    return None


def server_main(args):
    port = args.port
    state = MockState(args.seed, args.model_name, args.dim, port)
    stop_event = threading.Event()

    def request_shutdown():
        stop_event.set()

    handler = build_handler(state, request_shutdown)
    try:
        server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    except OSError as exc:
        sys.stderr.write("cannot bind 127.0.0.1:%d: %s\n" % (port, exc))
        return 1
    server.daemon_threads = True

    path = pid_file(port)
    try:
        with open(path, "w") as handle:
            handle.write("%d\n" % os.getpid())
    except (IOError, OSError):
        pass

    def on_signal(_signum, _frame):
        stop_event.set()

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    def waiter():
        stop_event.wait()
        server.shutdown()

    watcher = threading.Thread(target=waiter)
    watcher.daemon = True
    watcher.start()

    print("LISTENING %d" % port)
    sys.stdout.flush()
    try:
        server.serve_forever(poll_interval=0.05)
    finally:
        server.server_close()
        try:
            os.remove(path)
        except OSError:
            pass
        print("MOCK STOPPED")
        sys.stdout.flush()
    return 0


def build_parser():
    parser = argparse.ArgumentParser(
        description="deterministic mock AI service for AI COLUMN tests")
    parser.add_argument("--port", type=int, default=None,
                        help="listen port (default $AI_COLUMN_MOCK_PORT or 19120)")
    parser.add_argument("--model-name", default=DEFAULT_MODEL_NAME,
                        help="model name used when the request has none")
    parser.add_argument("--dim", type=int, default=None,
                        help="default embedding dimension (default $AI_COLUMN_MOCK_DIM or 4)")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED,
                        help="seed of the deterministic hash")
    parser.add_argument("--wait-ready", action="store_true",
                        help="client: wait until a server answers on the port")
    parser.add_argument("--wait-timeout", type=float, default=15.0,
                        help="client: seconds to wait in --wait-ready")
    parser.add_argument("--reset", action="store_true",
                        help="client: POST /__mock/reset")
    parser.add_argument("--config", default=None,
                        help="client: JSON patch for POST /__mock/config")
    parser.add_argument("--print-state", action="store_true",
                        help="client: print the one line state")
    parser.add_argument("--contains", default=None,
                        help="client: also count requests whose prompt contains TEXT")
    parser.add_argument("--assert-contains", default=None,
                        help="client: exit 0 only when a recorded prompt contains TEXT")
    parser.add_argument("--state-limit", type=int, default=DEFAULT_STATE_LIMIT,
                        help="client: how many requests to keep in the JSON state")
    parser.add_argument("--stop", action="store_true",
                        help="client: SIGTERM the server recorded in the pid file")
    parser.add_argument("--stop-if-running", action="store_true",
                        help="client: alias of --stop")
    return parser


def main(argv):
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.port is None:
        args.port = default_port()
    if args.dim is None:
        args.dim = default_dim()
    if args.port <= 0 or args.port > 65535:
        sys.stderr.write("bad port %r\n" % args.port)
        return 1
    if args.dim <= 0:
        sys.stderr.write("bad dimension %r\n" % args.dim)
        return 1

    client_result = client_main(args)
    if client_result is not None:
        return client_result
    return server_main(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
