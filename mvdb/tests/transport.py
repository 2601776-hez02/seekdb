#!/usr/bin/env python3
"""Shared JSONL transport for MiniVectorDB tests (official protocol).

Request:  {"id":N,"op":"execute","sql":"...","search_mode":"exact|ann"}
Response: {"id":N,"ok":true,...} / {"id":N,"ok":false,"error":{...}}
Handshake: {"type":"ready"} on startup.
"""
import json
import subprocess


class Db:
    def __init__(self, argv, data_dir, env=None):
        self.argv = list(argv)
        self.data_dir = data_dir
        self.env = env
        self.proc = None
        self.seq = 0

    def start(self, expect_ready=True):
        self.proc = subprocess.Popen(
            self.argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, universal_newlines=True, bufsize=1,
            env=self.env)
        if expect_ready:
            line = self.proc.stdout.readline()
            assert line, "no ready line"
            msg = json.loads(line)
            assert msg == {"type": "ready"}, msg

    def request(self, obj):
        self.seq += 1
        obj = dict(obj)
        obj.setdefault("id", self.seq)
        self.proc.stdin.write(json.dumps(obj, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        out = self.proc.stdout.readline()
        assert out, "no response for %r" % (obj,)
        resp = json.loads(out)
        assert type(resp["id"]) is type(obj["id"]) and resp["id"] == obj["id"], (obj, resp)
        return resp

    def execute(self, sql, mode="exact"):
        return self.request({"op": "execute", "sql": sql, "search_mode": mode})

    def send(self, sql, mode="exact"):
        return self.execute(sql, mode)

    def shutdown(self):
        resp = self.request({"op": "shutdown"})
        assert resp["ok"] is True, resp
        self.proc.stdin.close()
        self.proc.wait(timeout=60)
        assert self.proc.returncode == 0, self.proc.returncode
        extra = self.proc.stdout.read()
        assert extra == "", repr(extra)
        self.proc = None

    def kill9(self):
        if self.proc is not None:
            self.proc.kill()
            self.proc.wait()
            self.proc = None

    def stop(self):
        if self.proc is None or self.proc.poll() is not None:
            self.proc = None
            return
        try:
            self.shutdown()
        except Exception:
            if self.proc is not None and self.proc.poll() is None:
                self.proc.kill()
                self.proc.wait()
            self.proc = None
