#!/usr/bin/env python3
"""Full official-pattern rehearsal: fresh dir -> 100/batch writes -> index ->
mixed exact/ann queries -> clean shutdown -> restart persistence -> ann recall."""
import json
import os
import random
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from transport import Db  # noqa: E402

BINARY = "/root/mvdb/minivectordb" if len(sys.argv) < 2 else sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 200000
D = int(sys.argv[3]) if len(sys.argv) > 3 else 64
BATCH = 100
rng = random.Random(99)
D0 = tempfile.mkdtemp(prefix="mvdb-rehearsal-")


def start():
    db = Db([BINARY, "--data-dir", D0], D0)
    t0 = time.time()
    db.start()
    return db, time.time() - t0


def qvec():
    return "[" + ",".join("%.6f" % rng.uniform(-1, 1) for _ in range(D)) + "]"


db, dt = start()
print("start(empty): %.2fs" % dt)
r = db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(8), e VECTOR(%d));" % D)
assert r["ok"], r
t0 = time.time()
for b in range(1, N + 1, BATCH):
    end = min(b + BATCH, N + 1)
    rows = ",".join("(%d,'c%d','%s')" % (i, i % 7, qvec()) for i in range(b, end))
    r = db.send("INSERT INTO t VALUES %s;" % rows)
    assert r["ok"] and r["affected_rows"] == end - b, r
tw = time.time() - t0
print("write: %.2fs (%.0f rows/s)" % (tw, N / tw))
t0 = time.time()
r = db.send("CREATE VECTOR INDEX t_idx ON t(e);")
tb = time.time() - t0
assert r["ok"], r
print("index: %.2fs" % tb)
# ground truth (exact)
NQ = 30
truth = []
t0 = time.time()
for q in range(NQ):
    qv = qvec()
    r = db.send("SELECT id, L2_DISTANCE(e,'%s') AS d FROM t ORDER BY d LIMIT 10;" % qv)
    assert r["ok"], r
    truth.append((qv, [x[0] for x in r["rows"]]))
print("exact truth: %.2fs" % (time.time() - t0))
# ann mode
t0 = time.time()
rec = []
for qv, gt in truth:
    r = db.send("SELECT id, L2_DISTANCE(e,'%s') AS d FROM t ORDER BY d LIMIT 10;" % qv,
                mode="ann")
    assert r["ok"], r
    got = [x[0] for x in r["rows"]]
    assert len(got) == len(set(got)), "dup ids"
    rec.append(len(set(gt) & set(got)) / 10.0)
print("ann: %.2fs total, recall@10 mean %.4f min %.4f" %
      (time.time() - t0, sum(rec) / len(rec), min(rec)))
# filtered exact (ann mode still on)
t0 = time.time()
for q in range(10):
    qv = qvec()
    r = db.send("SELECT id, L2_DISTANCE(e,'%s') AS d FROM t WHERE c='c3' ORDER BY d LIMIT 10;" % qv,
                mode="ann")
    assert r["ok"], r
    ids = [x[0] for x in r["rows"]]
    assert len(ids) == len(set(ids)), "dup ids"
print("filtered: %.2fs (recall by construction 1.0)" % (time.time() - t0))
# persistence: clean shutdown + restart, verify rows + index fast-load
db.shutdown()
db, dt = start()
print("restart: %.2fs" % dt)
r = db.send("SELECT id FROM t ORDER BY id DESC LIMIT 1;")
assert r["ok"] and r["rows"][0][0] == N, r
r = db.send("SELECT id, L2_DISTANCE(e,'%s') AS d FROM t ORDER BY d LIMIT 10;" % truth[0][0])
assert r["ok"] and len(r["rows"]) == 10, r
db.shutdown()
print("REHEARSAL OK")
