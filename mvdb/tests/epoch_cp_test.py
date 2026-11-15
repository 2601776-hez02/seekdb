#!/usr/bin/env python3
"""Regression: checkpoint/flush early-return must not leave the epoch unbumped
(the vector-TopK filter cache would serve stale row positions).

Triggers for real: two ~3000-byte VARCHAR rows push the WAL past the lowered
checkpoint threshold (4096), and the checks run through the WHERE-filtered
vector TopK path (which uses the epoch-keyed bitmap cache)."""
import json
import os
import subprocess
import tempfile
import sys

BINARY = os.path.join(os.path.dirname(__file__), "..", "minivectordb")
if len(sys.argv) > 1:
    BINARY = sys.argv[1]

def run():
    d = tempfile.mkdtemp(prefix="mvdb-epochcp2-")
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
    send("CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(4000), e VECTOR(2));")
    big_a = "a" * 3000
    big_b = "b" * 3000
    # two records x ~3000B: the second commit crosses the 4096 threshold and
    # the checkpoint early-return path fires
    assert send("INSERT INTO t VALUES (1,'%s','[1,0]');" % big_a)["ok"]
    assert send("INSERT INTO t VALUES (2,'%s','[2,0]');" % big_b)["ok"]
    # build the epoch-keyed filter cache: a BIGINT predicate goes through the
    # bitmap-cache path (a VARCHAR equality would take the inline fast path)
    r = send("SELECT id, L2_DISTANCE(e,'[0,0]') AS d FROM t WHERE id=1 ORDER BY d LIMIT 5;")
    assert [x[0] for x in r["rows"]] == [1], r
    # delete id=1 (committed; the checkpoint path fires again on the WAL size)
    assert send("DELETE FROM t WHERE id=1;")["ok"]
    # the filtered query must now be EMPTY; a stale bitmap returns id=2
    r = send("SELECT id, L2_DISTANCE(e,'[0,0]') AS d FROM t WHERE id=1 ORDER BY d LIMIT 5;")
    assert r["rows"] == [], r
    # sanity: the full table still has exactly id=2
    r = send("SELECT id FROM t ORDER BY id;")
    assert [x[0] for x in r["rows"]] == [2], r
    # update id=2's id column... (updates go through the same apply path)
    assert send("UPDATE t SET c='%s' WHERE id=2;" % big_a)["ok"]
    r = send("SELECT id, L2_DISTANCE(e,'[0,0]') AS d FROM t WHERE id=2 ORDER BY d LIMIT 5;")
    assert [x[0] for x in r["rows"]] == [2], r
    p.stdin.write(json.dumps({"id": 999, "op": "shutdown"}) + "\n")
    p.stdin.flush()
    p.stdout.readline()
    p.wait(timeout=60)
    assert p.returncode == 0
    print("EPOCH-CHECKPOINT REGRESSION OK")

if __name__ == "__main__":
    run()
