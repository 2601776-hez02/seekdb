#!/usr/bin/env python3
"""Minimal Mock AI Service for the AI-function competition task.

Serves every protocol the AI_* functions speak, on 0.0.0.0:8000:

  POST /v1/chat/completions     OpenAI compatible chat completion
  POST /v1/embeddings           OpenAI compatible embeddings
  POST /api/generate            Ollama native completion
  POST /api/embed               Ollama native embedding
  POST /v1/rerank               SiliconFlow / Cohere style rerank
  POST /api/v1/services/rerank/text-rerank/text-rerank
                                DashScope text rerank
  GET  /__state                 request counters (debug aid)

Responses are deterministic so that expected mysqltest results can be derived:

* chat content  : ``mock: <last user message>``
* embeddings    : ``dimension`` floats where ``[i] = i + 1`` and
                  ``dimension`` comes from the request body (``dimensions`` or
                  ``dim[1]``) or defaults to 4
* ollama        : ``ollama: <prompt>`` and ``embeddings`` built the same way
* rerank        : score is the length of the document, so ordering is stable

Usage::

    python3 mock_ai_service.py [--port 8000] [--dim 4]
"""
import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = {"chat": 0, "embed": 0, "generate": 0, "ollama_embed": 0, "rerank": 0}
DEFAULT_DIM = 4


def vector(dim, seed=0):
    return [float(i + 1 + seed) for i in range(dim)]


def requested_dim(body, key="dimensions"):
    dim = body.get(key)
    if isinstance(dim, int) and dim > 0:
        return dim
    return DEFAULT_DIM


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # keep stdout readable
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send(self, payload, code=200):
        data = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/__state":
            self._send(STATE)
        else:
            self._send({"error": "not found"}, 404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        try:
            body = json.loads(raw or b"{}")
        except ValueError:
            self._send({"error": "bad json"}, 400)
            return

        path = self.path.split("?")[0]
        if path.endswith("/chat/completions"):
            STATE["chat"] += 1
            messages = body.get("messages") or []
            text = ""
            for message in messages:
                if message.get("role") == "user":
                    text = message.get("content") or ""
            self._send({
                "id": "chatcmpl-mock",
                "object": "chat.completion",
                "model": body.get("model", "mock"),
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant", "content": "mock: " + text},
                    "finish_reason": "stop",
                }],
                "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
            })
        elif path.endswith("/embeddings"):
            STATE["embed"] += 1
            inputs = body.get("input")
            if isinstance(inputs, str):
                inputs = [inputs]
            inputs = inputs or [""]
            dim = requested_dim(body)
            data = [{"object": "embedding", "index": i, "embedding": vector(dim, i)}
                    for i in range(len(inputs))]
            self._send({"object": "list", "model": body.get("model", "mock"),
                        "data": data, "usage": {"prompt_tokens": 1, "total_tokens": 1}})
        elif path.endswith("/api/generate"):
            STATE["generate"] += 1
            self._send({"model": body.get("model", "mock"),
                        "response": "ollama: " + (body.get("prompt") or ""),
                        "done": True})
        elif path.endswith("/api/embed"):
            STATE["ollama_embed"] += 1
            inputs = body.get("input")
            if isinstance(inputs, str):
                inputs = [inputs]
            inputs = inputs or [""]
            dim = requested_dim(body)
            self._send({"model": body.get("model", "mock"),
                        "embeddings": [vector(dim, i) for i in range(len(inputs))]})
        elif path.endswith("/rerank"):
            STATE["rerank"] += 1
            documents = body.get("documents") or []
            results = []
            for index, doc in enumerate(documents):
                text = doc if isinstance(doc, str) else json.dumps(doc, sort_keys=True)
                results.append({"index": index, "relevance_score": float(len(text)),
                                "document": doc})
            self._send({"id": "rerank-mock", "results": results,
                        "meta": {"tokens": 1}})
        else:
            self._send({"error": "unknown path: " + path}, 404)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--dim", type=int, default=4)
    args = parser.parse_args()
    global DEFAULT_DIM
    DEFAULT_DIM = args.dim
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print("mock AI service listening on %s:%d (default dim %d)"
          % (args.host, args.port, DEFAULT_DIM), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
