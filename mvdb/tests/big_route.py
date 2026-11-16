#!/usr/bin/env python3
"""Large-table numeric-safety gate regression (HNSW route is really entered).

Session 1 (gate on a READY graph): 100K small-value rows -> CREATE VECTOR
INDEX -> ann query #1 (must take the HNSW route) -> INSERT two huge rows
(bound jumps to 3e20) -> ann query #2 (must bypass the ready graph and return
the exact top-K of the small rows). Shutdown stats must show annHnsw == 1:
only query #1 used the graph.

Session 2 (ordering on a huge table): 100K rows whose distance order is the
REVERSE of the pk order; every float32 squared distance overflows. ann query
must return the double-reference top-K (the largest pks, not pk order).

Session 3 (bound survives a restart via run files): restart the huge table's
data dir (rows live in run files), CREATE INDEX again, ann query must still be
gated (exact top-K).

Routing evidence = the shutdown stats line (annHnsw/exact counters)."""
import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile

BINARY = sys.argv[1] if len(sys.argv) > 1 else "./minivectordb"
STATS_RE = re.compile(r"stats: queries=(\d+) exact=(\d+) annQuant=(\d+) annHnsw=(\d+)")


def f32(x):
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


class Conn:
    def __init__(self, binary, data_dir, log_path):
        self.argv = [binary, "--data-dir", data_dir]
        self.log_path = log_path
        self.proc = None
        self.seq = 0

    def start(self):
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(self.argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.log, universal_newlines=True, bufsize=1)
        line = self.proc.stdout.readline()
        assert line, "no ready line"
        assert json.loads(line) == {"type": "ready"}, line

    def send(self, sql, mode="exact"):
        self.seq += 1
        self.proc.stdin.write(json.dumps({"id": self.seq, "op": "execute", "sql": sql,
                                          "search_mode": mode}) + "\n")
        self.proc.stdin.flush()
        out = self.proc.stdout.readline()
        assert out, "no response for id %d" % self.seq
        resp = json.loads(out)
        assert resp["id"] == self.seq, resp
        return resp

    def stop(self):
        self.seq += 1
        self.proc.stdin.write(json.dumps({"id": self.seq, "op": "shutdown"}) + "\n")
        self.proc.stdin.flush()
        assert json.loads(self.proc.stdout.readline())["ok"] is True
        self.proc.stdin.close()
        rc = self.proc.wait(timeout=300)
        self.log.close()
        assert rc == 0, rc
        with open(self.log_path, "r", errors="replace") as f:
            txt = f.read()
        m = STATS_RE.search(txt)
        assert m, txt[-800:]
        return dict(queries=int(m.group(1)), exact=int(m.group(2)),
                    annQuant=int(m.group(3)), annHnsw=int(m.group(4)))


def topk(tbl, k=10):
    return ("SELECT id, L2_DISTANCE(e,'[0]') AS d FROM " + tbl +
            " ORDER BY d LIMIT %d;" % k)


def exact_ids(resp, rows, k, name):
    assert resp.get("ok"), (name, resp)
    ref = sorted(((pk, abs(v)) for pk, v in rows), key=lambda t: (t[1], t[0]))[:k]
    got = [(int(r[0]), float(r[1])) for r in resp["rows"]]
    assert [g[0] for g in got] == [x[0] for x in ref], (name, got, ref)
    for (gpk, gd), (rpk, rd) in zip(got, ref):
        assert gpk == rpk, (name, got, ref)
        assert abs(gd - rd) <= 1e-6 * max(1.0, abs(rd)), (name, gpk, gd, rd)
    return [g[0] for g in got]


def main():
    root = tempfile.mkdtemp(prefix="mvdb-bigroute-")
    N, K = 100000, 10
    small = [(i, f32(i * 1e-6)) for i in range(1, N + 1)]

    # ---- session 1: ready graph on small data, then the gate ----
    c = Conn(BINARY, root, os.path.join(root, "s1.log"))
    c.start()
    assert c.send("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(1));")["ok"]
    r = c.send("INSERT INTO t VALUES %s;" % ",".join("(%d, '[%r]')" % (i, i * 1e-6)
                                                     for i in range(1, N + 1)))
    assert r["ok"] and r["affected_rows"] == N, str(r)[:200]
    assert c.send("CREATE VECTOR INDEX t_idx ON t(e);")["ok"]
    r1 = c.send(topk("t"), mode="ann")
    assert r1.get("ok"), r1
    # two huge rows: the numeric bound jumps, the gate must bypass the graph
    r = c.send("INSERT INTO t VALUES (100001, '[3e20]'), (100002, '[2e20]');")
    assert r["ok"] and r["affected_rows"] == 2, r
    r2 = c.send(topk("t"), mode="ann")
    ids2 = exact_ids(r2, small + [(100001, f32(3e20)), (100002, f32(2e20))], K, "gated")
    assert ids2 == list(range(1, K + 1)), ids2
    st = c.stop()
    assert st["annHnsw"] == 1, ("expected only the pre-insert query on the graph", st)
    assert st["exact"] >= 1, st
    print("session1 OK: annHnsw=%d exact=%d (gate bypassed the ready graph)" % (st["annHnsw"], st["exact"]))

    # ---- session 2: 100K huge rows, distance order reversed vs pk ----
    c = Conn(BINARY, root, os.path.join(root, "s2.log"))
    c.start()
    assert c.send("CREATE TABLE big (id BIGINT PRIMARY KEY, e VECTOR(1));")["ok"]
    rows = [(i, 2e20 + (N - i) * 1e15) for i in range(1, N + 1)]
    r = c.send("INSERT INTO big VALUES %s;" % ",".join("(%d, '[%r]')" % (pk, v) for pk, v in rows))
    assert r["ok"] and r["affected_rows"] == N, str(r)[:200]
    assert c.send("CREATE VECTOR INDEX big_idx ON big(e);")["ok"]
    resp = c.send(topk("big"), mode="ann")
    ids = exact_ids(resp, [(pk, f32(v)) for pk, v in rows], K, "huge")
    assert ids == list(range(N, N - K, -1)), ids   # distance order, NOT pk order
    c.send(topk("big"), mode="ann")
    st = c.stop()
    assert st["annHnsw"] == 0, ("huge table used the graph", st)
    assert st["exact"] >= 2, st
    print("session2 OK: annHnsw=%d exact=%d ids=%s" % (st["annHnsw"], st["exact"], ids))

    # ---- session 3: 40K huge rows; small-table index (quant snapshot) is
    #      built synchronously; the gate must bypass it as well ----
    M = 40000
    rows3 = [(i, 2e20 + (M - i) * 1e15) for i in range(1, M + 1)]
    c = Conn(BINARY, root, os.path.join(root, "s3.log"))
    c.start()
    assert c.send("CREATE TABLE mid (id BIGINT PRIMARY KEY, e VECTOR(1));")["ok"]
    r = c.send("INSERT INTO mid VALUES %s;" % ",".join("(%d, '[%r]')" % (pk, v) for pk, v in rows3))
    assert r["ok"] and r["affected_rows"] == M, str(r)[:200]
    assert c.send("CREATE VECTOR INDEX mid_idx ON mid(e);")["ok"]
    resp = c.send(topk("mid"), mode="ann")
    ids3 = exact_ids(resp, [(pk, f32(v)) for pk, v in rows3], K, "mid")
    assert ids3 == list(range(M, M - K, -1)), ids3
    st = c.stop()
    assert st["annHnsw"] == 0 and st["annQuant"] == 0, ("gate did not bypass the small-table index", st)
    print("session3 OK: annHnsw=%d annQuant=%d exact=%d ids=%s" % (st["annHnsw"], st["annQuant"], st["exact"], ids3))

    # ---- session 4: restart; the engine rebuilds the small-table index at
    #      startup, so the numeric bound MUST be restored from the recovered
    #      rows (WAL replay) for the gate to fire again ----
    c = Conn(BINARY, root, os.path.join(root, "s4.log"))
    c.start()
    resp = c.send(topk("mid"), mode="ann")
    ids4 = exact_ids(resp, [(pk, f32(v)) for pk, v in rows3], K, "restart")
    assert ids4 == list(range(M, M - K, -1)), ids4
    st = c.stop()
    assert st["annHnsw"] == 0 and st["annQuant"] == 0, ("restart bound missing -> index used", st)
    print("session4 OK: annHnsw=%d annQuant=%d exact=%d ids=%s (bound restored)" % (st["annHnsw"], st["annQuant"], st["exact"], ids4))
    print("BIG-ROUTE GATE OK (route / gate-on-ready-graph / ordering / restart-bound)")


if __name__ == "__main__":
    main()
