#!/usr/bin/env python3
"""Scale smoke: batch writes + HNSW build + ANN recall/timing."""
import json
import math
import os
import random
import subprocess
import sys
import tempfile
import time

BINARY = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 200000
D = int(sys.argv[3]) if len(sys.argv) > 3 else 64
K = 10
BATCH = 1000
NQ = 100

rng = random.Random(42)


def gen_vec(d):
    return "[" + ",".join("%.6f" % rng.uniform(-1, 1) for _ in range(d)) + "]"


class Db:
    def __init__(self, data_dir):
        self.data_dir = data_dir
        self.proc = None
        self.seq = 0

    def start(self):
        self.proc = subprocess.Popen(
            [BINARY, "--data-dir", self.data_dir],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            universal_newlines=True, bufsize=1)
        line = self.proc.stdout.readline()
        assert json.loads(line) == {"type": "ready"}, line

    def send(self, sql, mode="exact"):
        t0 = time.time()
        self.seq += 1
        req = {"id": self.seq, "op": "execute", "sql": sql, "search_mode": mode}
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        out = self.proc.stdout.readline()
        assert out, "no response"
        return json.loads(out), time.time() - t0

    def kill(self):
        self.proc.kill()
        self.proc.wait()


d = tempfile.mkdtemp(prefix="mvdb-bench-")
db = Db(d)
db.start()
print("== schema")
r, _ = db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(8), e VECTOR(%d));" % D)
assert r["ok"], r

print("== writing %d rows x %dd in batches of %d" % (N, D, BATCH))
t0 = time.time()
for start in range(1, N + 1, BATCH):
    end = min(start + BATCH, N + 1)
    rows = ",".join("(%d,'c%d','%s')" % (i, i % 7, gen_vec(D)) for i in range(start, end))
    r, _ = db.send("INSERT INTO t VALUES %s;" % rows)
    assert r["ok"] and r["affected_rows"] == end - start, (r, start)
tw = time.time() - t0
print("write time: %.2fs (%.0f rows/s)" % (tw, N / tw))

print("== creating vector index")
r, ti = db.send("CREATE VECTOR INDEX t_idx ON t(e);")
assert r["ok"], r
print("index build time: %.2fs" % ti)

print("== exact ground truth for %d queries" % NQ)
truth = []
t0 = time.time()
for q in range(NQ):
    qv = gen_vec(D)
    r, _ = db.send("SELECT id, L2_DISTANCE(e,'%s') AS d FROM t ORDER BY d LIMIT %d;" % (qv, K))
    truth.append((qv, [x[0] for x in r["rows"]]))
texact = time.time() - t0
print("exact query time: %.2fs total, %.1f ms/query" % (texact, texact / NQ * 1000))

print("== ann queries")
t0 = time.time()
recalls = []
for qv, gt in truth:
    r, _ = db.send("SELECT id, L2_DISTANCE(e,'%s') AS d FROM t ORDER BY d LIMIT %d;" % (qv, K),
                   mode="ann")
    got = [x[0] for x in r["rows"]]
    inter = len(set(gt) & set(got))
    recalls.append(inter / float(K))
tann = time.time() - t0
print("ann query time: %.2fs total, %.1f ms/query" % (tann, tann / NQ * 1000))
print("recall@%d: mean %.4f, min %.4f" % (K, sum(recalls) / len(recalls), min(recalls)))

print("== filtered ann queries (c='c3', selectivity 1/7)")
t0 = time.time()
fanns = []
for q in range(NQ):
    qv = gen_vec(D)
    r, _ = db.send("SELECT id, L2_DISTANCE(e,'%s') AS d FROM t WHERE c='c3' ORDER BY d LIMIT %d;" % (qv, K),
                   mode="ann")
    fanns.append(([x[0] for x in r["rows"]], qv))
tfann = time.time() - t0
t0 = time.time()
frec = []
for got, qv in fanns:
    r, _ = db.send("SELECT id, L2_DISTANCE(e,'%s') AS d FROM t WHERE c='c3' ORDER BY d LIMIT %d;" % (qv, K))
    gt = [x[0] for x in r["rows"]]
    inter = len(set(gt) & set(got))
    frec.append(inter / float(K))
texf = time.time() - t0
print("filtered ann: %.2fs (%.1f ms/q), exact filtered: %.2fs, recall@%d mean %.4f min %.4f"
      % (tfann, tfann / NQ * 1000, texf, K, sum(frec) / len(frec), min(frec)))
db.kill()
print("done; data dir:", d)
