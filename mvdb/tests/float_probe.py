#!/usr/bin/env python3
"""Float-parser edge probes via the DB (fast parser must match strtod semantics
for the float32-critical cases)."""
import json
import subprocess
import sys
import tempfile

BINARY = sys.argv[1]
D = tempfile.mkdtemp(prefix="mvdb-fp-")
p = subprocess.Popen([BINARY, "--data-dir", D],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, universal_newlines=True, bufsize=1)
assert json.loads(p.stdout.readline()) == {"type": "ready"}
seq = [0]


def send(sql):
    seq[0] += 1
    p.stdin.write(json.dumps({"id": seq[0], "op": "execute", "sql": sql,
                              "search_mode": "exact"}) + "\n")
    p.stdin.flush()
    return json.loads(p.stdout.readline())


assert send("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2));")["ok"]
# float32-critical: 16777217 rounds to 16777216 in float32 -> distance 0;
# 16777218 stays distinct -> distance 2
assert send("INSERT INTO t VALUES (1,'[16777216,0]'),(2,'[16777217,0]');")["affected_rows"] == 2
r = send("SELECT L2_DISTANCE(e,'[16777217,0]') AS d FROM t WHERE id=2;")
assert r["ok"] and abs(r["rows"][0][0]) < 1e-6, r
r = send("SELECT L2_DISTANCE(e,'[16777217,0]') AS d FROM t WHERE id=1;")
assert r["ok"] and abs(r["rows"][0][0]) < 1e-6, r   # both round to the same f32
assert send("INSERT INTO t VALUES (10,'[16777218,0]');")["affected_rows"] == 1
r = send("SELECT L2_DISTANCE(e,'[16777216,0]') AS d FROM t WHERE id=10;")
assert r["ok"] and abs(r["rows"][0][0] - 2.0) < 1e-6, r
# plain decimals / signs / exponents / spaces
r = send("SELECT L2_DISTANCE(e,'[3,4]') AS d FROM t WHERE id=1;")
assert r["ok"] and abs(r["rows"][0][0] - 16777213.0) < 1e-2, r
assert send("INSERT INTO t VALUES (3,'[+0.1,-0.1]');")["affected_rows"] == 1
r = send("SELECT L2_DISTANCE(e,'[0.1,-0.1]') AS d FROM t WHERE id=3;")
assert r["ok"] and abs(r["rows"][0][0]) < 1e-6, r
assert send("INSERT INTO t VALUES (4,'[1e2,2E-2]');")["affected_rows"] == 1
r = send("SELECT L2_DISTANCE(e,'[100,0.02]') AS d FROM t WHERE id=4;")
assert r["ok"] and abs(r["rows"][0][0]) < 1e-6, r
# underflow to 0 accepted (old semantics), overflow rejected
assert send("INSERT INTO t VALUES (5,'[1e-999,0]');")["affected_rows"] == 1
r = send("SELECT L2_DISTANCE(e,'[0,0]') AS d FROM t WHERE id=5;")
assert r["ok"] and abs(r["rows"][0][0]) < 1e-6, r
r = send("INSERT INTO t VALUES (6,'[1e999,0]');")
assert not r["ok"] and r["error"]["code"] == "invalid_vector", r
r = send("INSERT INTO t VALUES (7,'[1e40,0]');")   # > float32 max
assert not r["ok"] and r["error"]["code"] == "invalid_vector", r
# junk rejected
for bad in ["'[1x,2]'", "'[1..2]'", "'[1 2]'", "'[.5,1]'"]:
    r = send("INSERT INTO t VALUES (8,'%s');" % bad)
    assert not r["ok"], (bad, r)
# trailing comma tolerated
r = send("INSERT INTO t VALUES (9,'[1,2,]');")
assert r["ok"] and r["affected_rows"] == 1, r
r = send("SELECT L2_DISTANCE(e,'[1,2]') AS d FROM t WHERE id=9;")
assert r["ok"] and abs(r["rows"][0][0]) < 1e-6, r
p.stdin.write(json.dumps({"id": 999, "op": "shutdown"}) + "\n")
p.stdin.flush()
p.stdout.readline()
p.wait(timeout=30)
print("FLOAT PARSE PROBES OK")
