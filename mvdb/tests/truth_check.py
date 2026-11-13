#!/usr/bin/env python3
"""Independent ground-truth verification of the exact TopK at scale:
compare the DB's answers against a from-scratch Python reference
(float32 rounding via struct, distances in double, ties by pk ascending)."""
import json
import random
import struct
import subprocess
import sys
import tempfile

BINARY = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 200000
D = int(sys.argv[3]) if len(sys.argv) > 3 else 64
NQ = 100
K = 10
rng = random.Random(123)


def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def gen(d):
    return ["%.6f" % rng.uniform(-1, 1) for _ in range(d)]


# ground-truth dataset: keep per-row float32 vectors
data = {}   # id -> list of f32 floats
for i in range(1, N + 1):
    data[i] = [f32(float(s)) for s in gen(D)]

queries = [[f32(float(s)) for s in gen(D)] for _ in range(NQ)]

D0 = tempfile.mkdtemp(prefix="mvdb-truth-")
p = subprocess.Popen([BINARY, "--data-dir", D0],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, universal_newlines=True, bufsize=1)
assert json.loads(p.stdout.readline()) == {"type": "ready"}
seq = [0]


def send(sql, mode="exact"):
    seq[0] += 1
    p.stdin.write(json.dumps({"id": seq[0], "op": "execute", "sql": sql,
                              "search_mode": mode}) + "\n")
    p.stdin.flush()
    return json.loads(p.stdout.readline())


assert send("CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(8), e VECTOR(%d));" % D)["ok"]
BATCH = 1000
for b in range(1, N + 1, BATCH):
    end = min(b + BATCH, N + 1)
    rows = ",".join("(%d,'c%d','[%s]')" % (i, i % 7, ",".join(map(str, data[i])))
                    for i in range(b, end))
    r = send("INSERT INTO t VALUES %s;" % rows)
    assert r["ok"] and r["affected_rows"] == end - b, r
print("loaded %d rows" % N)

bad = 0
for q, qv in enumerate(queries):
    # independent ground truth
    scored = []
    for i, v in data.items():
        s = 0.0
        for a, b in zip(v, qv):
            dd = a - b
            s += dd * dd
        scored.append((s, i))
    scored.sort()
    gt = [x[1] for x in scored[:K]]
    qs = "[" + ",".join(map(str, qv)) + "]"
    r = send("SELECT id, L2_DISTANCE(e,'%s') AS d FROM t ORDER BY d LIMIT %d;" % (qs, K))
    assert r["ok"], r
    got = [x[0] for x in r["rows"]]
    if got != gt:
        bad += 1
        print("MISMATCH q=%d\ngt =%s\ngot=%s" % (q, gt[:12], got[:12]))
        if bad >= 3:
            break
    if q % 20 == 0:
        print("verified %d/%d" % (q, NQ))
print("INDEPENDENT TRUTH:", "OK (%d queries)" % NQ if bad == 0 else "%d MISMATCHES" % bad)
p.stdin.write(json.dumps({"id": 999, "op": "shutdown"}) + "\n")
p.stdin.flush()
p.stdout.readline()
p.wait(timeout=60)
