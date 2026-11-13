#!/usr/bin/env python3
"""Hybrid-gate correctness at scale:
1. 200Kx64d pure inserts -> filtered TopK (c='c3') must match an independent
   Python reference exactly (exercises the parallel inline-predicate path).
2. UPDATE + DELETE some rows -> the overlapping-runs gate must fall back to
   the merged scan; filtered AND unfiltered results still match the
   reference after the mutations."""
import json
import random
import struct
import subprocess
import tempfile

BINARY = "/root/mvdb/minivectordb"
N = 200000
D = 64
BATCH = 1000
rng = random.Random(311)

def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]

def gen_vec(d):
    return [f32(rng.uniform(-1, 1)) for _ in range(d)]

# ground truth: pk -> (c, vec)
data = {}
for i in range(1, N + 1):
    data[i] = ("c%d" % (i % 7), gen_vec(D))

qs = [gen_vec(D) for _ in range(20)]

def expected_topk(flt, q):
    scored = []
    for pk, (c, v) in data.items():
        if flt is not None and c != flt:
            continue
        s = 0.0
        for a, b in zip(v, q):
            dd = a - b
            s += dd * dd
        scored.append((s, pk))
    scored.sort()
    return [x[1] for x in scored[:10]]

d = tempfile.mkdtemp(prefix="mvdb-hyb-")
p = subprocess.Popen([BINARY, "--data-dir", d], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                     universal_newlines=True, bufsize=1 << 20)
assert json.loads(p.stdout.readline()) == {"type": "ready"}
seq = [0]
def send(sql, mode="exact"):
    seq[0] += 1
    p.stdin.write(json.dumps({"id": seq[0], "op": "execute", "sql": sql,
                              "search_mode": mode}) + "\n")
    p.stdin.flush()
    return json.loads(p.stdout.readline())

send("CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(8), e VECTOR(%d));" % D)
for b in range(1, N + 1, BATCH):
    end = min(b + BATCH, N + 1)
    rows = ",".join("(%d,'%s','[%s]')" % (i, data[i][0], ",".join(map(str, data[i][1])))
                    for i in range(b, end))
    r = send("INSERT INTO t VALUES %s;" % rows)
    assert r["ok"], r
print("loaded %d rows" % N)

# unfiltered + filtered before mutations
bad = 0
for qi, qv in enumerate(qs):
    qs_str = "[" + ",".join(map(str, qv)) + "]"
    r1 = send("SELECT id, L2_DISTANCE(e,'%s') AS d FROM t ORDER BY d LIMIT 10;" % qs_str)
    if [x[0] for x in r1["rows"]] != expected_topk(None, qv):
        bad += 1
        print("MISMATCH unfiltered q=%d" % qi)
    r2 = send("SELECT id, L2_DISTANCE(e,'%s') AS d FROM t WHERE c='c3' ORDER BY d LIMIT 10;" % qs_str)
    if [x[0] for x in r2["rows"]] != expected_topk("c3", qv):
        bad += 1
        print("MISMATCH filtered q=%d" % qi)
print("pre-mutation checks:", "OK" if bad == 0 else "%d MISMATCHES" % bad)
assert bad == 0

# mutations: update ~200 rows, delete ~200 rows (make runs overlap)
for i in range(1, 40001, 200):
    c = "c9"
    v = gen_vec(D)
    data[i] = (c, v)
    r = send("UPDATE t SET c='%s', e='[%s]' WHERE id=%d;" % (c, ",".join(map(str, v)), i))
    assert r["ok"], r
for i in range(2, 40002, 200):
    del data[i]
    r = send("DELETE FROM t WHERE id=%d;" % i)
    assert r["ok"], r
print("mutated 200 rows (update) + 200 rows (delete)")

bad = 0
for qi, qv in enumerate(qs):
    qs_str = "[" + ",".join(map(str, qv)) + "]"
    r1 = send("SELECT id, L2_DISTANCE(e,'%s') AS d FROM t ORDER BY d LIMIT 10;" % qs_str)
    if [x[0] for x in r1["rows"]] != expected_topk(None, qv):
        bad += 1
        print("MISMATCH unfiltered-after q=%d" % qi)
    r2 = send("SELECT id, L2_DISTANCE(e,'%s') AS d FROM t WHERE c='c3' ORDER BY d LIMIT 10;" % qs_str)
    if [x[0] for x in r2["rows"]] != expected_topk("c3", qv):
        bad += 1
        print("MISMATCH filtered-after q=%d" % qi)
    r3 = send("SELECT id, L2_DISTANCE(e,'%s') AS d FROM t WHERE c='c9' ORDER BY d LIMIT 10;" % qs_str)
    if [x[0] for x in r3["rows"]] != expected_topk("c9", qv):
        bad += 1
        print("MISMATCH filtered-c9-after q=%d" % qi)
print("post-mutation checks:", "OK" if bad == 0 else "%d MISMATCHES" % bad)
assert bad == 0
print("HYBRID GATE CORRECTNESS OK")
p.stdin.write(json.dumps({"id": 999, "op": "shutdown"}) + "\n")
p.stdin.flush()
p.stdout.readline()
p.wait(timeout=60)
