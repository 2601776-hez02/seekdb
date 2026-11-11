#!/usr/bin/env python3
"""Crash fuzz: tiny flush/checkpoint thresholds + random kill points, then
verify statement atomicity and durability across restarts (with an index).

Usage: python3 fuzz_crash.py <binary> [iterations]
"""
import json
import os
import random
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from transport import Db  # noqa: E402

BINARY = sys.argv[1]
ITERS = int(sys.argv[2]) if len(sys.argv) > 2 else 12
ENV = dict(os.environ)
ENV["MVDB_MEMTABLE_BYTES"] = "65536"          # 64 KiB
ENV["MVDB_WAL_CHECKPOINT_BYTES"] = "131072"   # 128 KiB

D = tempfile.mkdtemp(prefix="mvdb-fuzz-")
rng = random.Random(7)


def start():
    db = Db([BINARY, "--data-dir", D], D, env=ENV)
    db.start()
    return db


def max_id(db):
    r = db.send("SELECT id FROM t ORDER BY id DESC LIMIT 1;")
    if not r["ok"] or not r["rows"]:
        return 0
    return r["rows"][0][0]


def topk_check(db):
    r = db.send("SELECT id, L2_DISTANCE(e,'[1000,0]') AS d FROM t ORDER BY d LIMIT 2;")
    assert r["ok"], r
    return [(x[0], round(x[1], 1)) for x in r["rows"]]


def main():
    db = start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(8), e VECTOR(2));")["ok"]
    assert db.send("CREATE VECTOR INDEX t_idx ON t(e);")["ok"]
    db.kill9()

    BATCH = 500
    expected = 0
    for it in range(ITERS):
        db = start()
        got = max_id(db)
        assert got == expected, "iteration %d: expected max_id %d, got %d" % (it, expected, got)
        if expected > 0:
            tk = topk_check(db)
            assert tk[0][0] == min(expected, 500), tk
        rows = ",".join("(%d,'c%d','[%d,%d]')" % (i, i % 7, i, -i)
                        for i in range(expected + 1, expected + BATCH + 1))
        stmt = "INSERT INTO t VALUES %s;" % rows
        r = db.send(stmt)
        assert r["ok"] and r["affected_rows"] == BATCH, r
        # kill at a random point after the commit response (covers flush/checkpoint
        # windows of the NEXT statements via a follow-up update+delete pair)
        time.sleep(rng.uniform(0, 0.15))
        try:
            db.send("UPDATE t SET c = 'u' WHERE id = %d;" % (expected + 1))
            db.send("DELETE FROM t WHERE id = %d;" % (expected + 2))
        except Exception:
            pass
        db.kill9()
        # after recovery: the batch is all-or-nothing; the follow-up update/delete
        # are committed or not, but the batch rows must be fully present
        db2 = start()
        got = max_id(db2)
        assert got == expected + BATCH or got == expected, \
            "iteration %d: atomicity broken: max_id %d (expected %d or %d)" % (
                it, got, expected, expected + BATCH)
        r = db2.send("SELECT id FROM t WHERE id = %d;" % (expected + 1))
        assert len(r["rows"]) == 1, r
        if got == expected + BATCH:
            expected += BATCH
        db2.kill9()
        print("iteration %d ok (max_id=%d)" % (it, got))

    db = start()
    assert max_id(db) == expected
    print("fuzz done, final max_id=%d" % expected)
    db.kill9()
    print("OK")


if __name__ == "__main__":
    main()
