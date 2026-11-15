#!/usr/bin/env python3
"""Regression: checkpoint early-return must not leave the epoch unbumped
(the filter cache would serve stale row positions). Reviewer's repro with a
low WAL checkpoint threshold (default is 64MiB; 4096 forces the path)."""
import json
import os
import subprocess
import tempfile

BINARY = os.path.join(os.path.dirname(__file__), "..", "minivectordb")

def run():
    d = tempfile.mkdtemp(prefix="mvdb-epochcp-")
    env = dict(os.environ)
    env["MVDB_WAL_CHECKPOINT_BYTES"] = "4096"
    p = subprocess.Popen([BINARY, "--data-dir", d], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         universal_newlines=True, bufsize=1 << 20, env=env)
    assert json.loads(p.stdout.readline()) == {"type": "ready"}
    seq = [0]
    def send(sql, mode="exact"):
        seq[0] += 1
        p.stdin.write(json.dumps({"id": seq[0], "op": "execute", "sql": sql,
                                  "search_mode": mode}) + "\n")
        p.stdin.flush()
        return json.loads(p.stdout.readline())
    send("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2));")
    assert send("INSERT INTO t VALUES (1,'[1,0]'),(2,'[2,0]');")["ok"]
    # build the filter cache
    r = send("SELECT id FROM t WHERE id=1;")
    assert [x[0] for x in r["rows"]] == [1], r
    # delete id=1 (committed mutation + possible checkpoint early-return)
    assert send("DELETE FROM t WHERE id=1;")["ok"]
    r = send("SELECT id FROM t WHERE id=1;")
    assert r["rows"] == [], r
    # the full set must be just id=2
    r = send("SELECT id FROM t ORDER BY id;")
    assert [x[0] for x in r["rows"]] == [2], r
    # update path too: update id=2's e and re-check the filtered path
    assert send("UPDATE t SET e='[9,9]' WHERE id=2;")["ok"]
    r = send("SELECT id FROM t WHERE id=2;")
    assert [x[0] for x in r["rows"]] == [2], r
    p.stdin.write(json.dumps({"id": 999, "op": "shutdown"}) + "\n")
    p.stdin.flush()
    p.stdout.readline()
    p.wait(timeout=60)
    assert p.returncode == 0
    print("EPOCH-CHECKPOINT REGRESSION OK")

if __name__ == "__main__":
    run()
