#!/usr/bin/env python3
"""Crash-recovery and robustness tests for minivectordb (official JSONL protocol)."""
import json
import os
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from transport import Db as _Base  # noqa: E402

BINARY = sys.argv[1]


class Db(_Base):
    def __init__(self, data_dir):
        super().__init__([BINARY, "--data-dir", data_dir], data_dir)


def test_committed_survives_kill9():
    print("== committed data survives kill -9, uncommitted txn discarded")
    d = tempfile.mkdtemp(prefix="mvdb-crash-")
    db = Db(d)
    db.start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(16), e VECTOR(2));")["ok"]
    rows = ",".join("(%d,'r%d','[%d,0]')" % (i, i, i) for i in range(1, 21))
    assert db.send("INSERT INTO t VALUES %s;" % rows)["affected_rows"] == 20
    assert db.send("BEGIN;")["ok"]
    assert db.send("DELETE FROM t WHERE id <= 10;")["affected_rows"] == 10
    assert db.send("INSERT INTO t VALUES (21,'x','[1,1]');")["affected_rows"] == 1
    # kill mid-txn: deletes + insert must vanish, autocommitted 20 rows stay
    db.kill9()
    db.start()
    r = db.send("SELECT id FROM t ORDER BY id;")
    got = [x[0] for x in r["rows"]]
    assert got == list(range(1, 21)), got
    db.stop()
    print("  ok")


def test_kill9_mid_autocommit_batch():
    print("== kill -9 right after autocommit batch: batch atomic")
    d = tempfile.mkdtemp(prefix="mvdb-crash2-")
    db = Db(d)
    db.start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(16));")["ok"]
    for i in range(1, 10):
        assert db.send("INSERT INTO t VALUES (%d,'r%d');" % (i, i))["affected_rows"] == 1
    # now kill immediately after sending a batch (no chance to fsync)
    db.seq += 1
    db.proc.stdin.write(json.dumps(
        {"id": db.seq, "op": "execute",
         "sql": "INSERT INTO t VALUES (11,'a'),(12,'b');",
         "search_mode": "exact"}) + "\n")
    db.proc.stdin.flush()
    time.sleep(0.05)
    db.kill9()
    db.start()
    r = db.send("SELECT id FROM t ORDER BY id;")
    got = [x[0] for x in r["rows"]]
    # either 0 or 2 new rows, never 1 (statement atomicity)
    base = list(range(1, 10))
    assert got == base or got == base + [11, 12], got
    db.stop()
    print("  ok")


def test_invalid_vectors():
    print("== NaN/Inf/overflow/dim errors")
    d = tempfile.mkdtemp(prefix="mvdb-vec-")
    db = Db(d)
    db.start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2));")["ok"]
    for bad, err in [
        ("'[1,NaN]'", "invalid_vector"),
        ("'[1,Infinity]'", "invalid_vector"),
        ("'[1,-Infinity]'", "invalid_vector"),
        ("'[1e999,0]'", "invalid_vector"),
        ("'[1]'", "dimension_mismatch"),
        ("'[1,2,3]'", "dimension_mismatch"),
        ("'[1,2'", "invalid_vector"),
        ("'foo'", "invalid_vector"),
    ]:
        r = db.send("INSERT INTO t VALUES (1,%s);" % bad)
        assert not r["ok"] and r["error"]["code"] == err, (bad, r)
    r = db.send("SELECT id FROM t;")
    assert r["rows"] == []
    # varchar unicode length
    assert db.send("CREATE TABLE u (id BIGINT PRIMARY KEY, c VARCHAR(3));")["ok"]
    assert db.send("INSERT INTO u VALUES (1,'中文三');")["affected_rows"] == 1
    r = db.send("INSERT INTO u VALUES (2,'中文四个');")
    assert not r["ok"] and r["error"]["code"] == "string_too_long", r
    # bigint overflow
    r = db.send("INSERT INTO u VALUES (3,'x');")  # ok
    r = db.send("INSERT INTO t VALUES (99999999999999999999,'[1,2]');")
    assert not r["ok"], r
    db.stop()
    print("  ok")


def test_partial_wal_tail():
    print("== torn WAL tail is truncated on recovery")
    d = tempfile.mkdtemp(prefix="mvdb-torn-")
    db = Db(d)
    db.start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(16));")["ok"]
    for i in range(1, 6):
        assert db.send("INSERT INTO t VALUES (%d,'r%d');" % (i, i))["affected_rows"] == 1
    db.stop()
    # append garbage to wal.log
    with open(os.path.join(d, "wal.log"), "ab") as f:
        f.write(b"\x4d\x57\x41\x4c" + b"\xff\xff\xff\xff" + b"garbagegarbage")
    db.start()
    r = db.send("SELECT id FROM t ORDER BY id;")
    assert [x[0] for x in r["rows"]] == [1, 2, 3, 4, 5]
    db.stop()
    print("  ok")


def test_ann_recall_small():
    print("== ann mode returns exact results on small data + filter")
    d = tempfile.mkdtemp(prefix="mvdb-ann-")
    db = Db(d)
    db.start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(4), e VECTOR(4));")["ok"]
    rows = ",".join("(%d,'%s','[%d,0,0,0]')" % (i, "keep" if i % 2 else "skip", i)
                    for i in range(1, 101))
    assert db.send("INSERT INTO t VALUES %s;" % rows)["affected_rows"] == 100
    assert db.send("CREATE VECTOR INDEX t_idx ON t(e);")["ok"]
    # exact mode
    r = db.send("SELECT id, L2_DISTANCE(e,'[1,0,0,0]') AS d FROM t ORDER BY d LIMIT 3;")
    exact = [(x[0], x[1]) for x in r["rows"]]
    # ann mode (per-request search_mode)
    r = db.send("SELECT id, L2_DISTANCE(e,'[1,0,0,0]') AS d FROM t ORDER BY d LIMIT 3;", mode="ann")
    ann = [(x[0], x[1]) for x in r["rows"]]
    assert ann == exact, (ann, exact)
    # filtered ann
    r = db.send("SELECT id, L2_DISTANCE(e,'[5,0,0,0]') AS d FROM t WHERE c='keep' ORDER BY d LIMIT 2;",
                mode="ann")
    got = [(x[0], x[1]) for x in r["rows"]]
    assert got == [(5, 0.0), (3, 2.0)], got
    db.stop()
    print("  ok")


def test_index_maintenance_txn():
    print("== index correctness across update/delete/restart inside txn")
    d = tempfile.mkdtemp(prefix="mvdb-im-")
    db = Db(d)
    db.start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2));")["ok"]
    assert db.send("INSERT INTO t VALUES (1,'[1,0]'),(2,'[2,0]'),(3,'[3,0]');")["affected_rows"] == 3
    assert db.send("CREATE VECTOR INDEX t_idx ON t(e);")["ok"]
    assert db.send("BEGIN;")["ok"]
    assert db.send("UPDATE t SET e='[0,1]' WHERE id=1;")["affected_rows"] == 1
    assert db.send("DELETE FROM t WHERE id=2;")["affected_rows"] == 1
    r = db.send("SELECT id, L2_DISTANCE(e,'[0,0]') AS d FROM t ORDER BY d LIMIT 5;")
    got = [(x[0], x[1]) for x in r["rows"]]
    assert got == [(1, 1.0), (3, 3.0)], got
    assert db.send("ROLLBACK;")["ok"]
    r = db.send("SELECT id, L2_DISTANCE(e,'[0,0]') AS d FROM t ORDER BY d LIMIT 5;")
    got = [(x[0], x[1]) for x in r["rows"]]
    assert got == [(1, 1.0), (2, 2.0), (3, 3.0)], got
    assert db.send("BEGIN;")["ok"]
    assert db.send("DELETE FROM t WHERE id=3;")["affected_rows"] == 1
    assert db.send("COMMIT;")["ok"]
    db.stop()
    db.start()
    r = db.send("SELECT id, L2_DISTANCE(e,'[0,0]') AS d FROM t ORDER BY d LIMIT 5;")
    got = [(x[0], x[1]) for x in r["rows"]]
    assert got == [(1, 1.0), (2, 2.0)], got
    db.stop()
    print("  ok")


def test_where_unknown_column():
    print("== WHERE on unknown column -> column_not_found")
    d = tempfile.mkdtemp(prefix="mvdb-uc-")
    db = Db(d)
    db.start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v BIGINT);")["ok"]
    assert db.send("INSERT INTO t VALUES (1,10),(2,20);")["affected_rows"] == 2
    for sql in ["SELECT id FROM t WHERE nope = 1;",
                "UPDATE t SET v = 1 WHERE nope = 1;",
                "DELETE FROM t WHERE nope = 1;"]:
        r = db.send(sql)
        assert not r["ok"] and r["error"]["code"] == "column_not_found", (sql, r)
    r = db.send("SELECT id FROM t ORDER BY id;")
    assert [x[0] for x in r["rows"]] == [1, 2]
    db.stop()
    print("  ok")


def test_update_unchanged_not_counted():
    print("== UPDATE to same value -> affected_rows 0 (MySQL semantics)")
    d = tempfile.mkdtemp(prefix="mvdb-uc2-")
    db = Db(d)
    db.start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v BIGINT, s VARCHAR(8));")["ok"]
    assert db.send("INSERT INTO t VALUES (1,10,'a'),(2,20,'b');")["affected_rows"] == 2
    r = db.send("UPDATE t SET v = v WHERE id = 1;")
    # (v=v is unsupported column-ref syntax; use a literal equal to the value)
    assert not r["ok"], r
    r = db.send("UPDATE t SET v = 10 WHERE id = 1;")
    assert r["ok"] and r["affected_rows"] == 0, r
    r = db.send("UPDATE t SET v = 10 WHERE id = 2;")
    assert r["ok"] and r["affected_rows"] == 1, r
    r = db.send("UPDATE t SET v = 11 WHERE id >= 1;")
    assert r["ok"] and r["affected_rows"] == 2, r
    db.stop()
    print("  ok")


def test_duplicate_column_create():
    print("== CREATE TABLE with duplicate column -> syntax_error")
    d = tempfile.mkdtemp(prefix="mvdb-dc-")
    db = Db(d)
    db.start()
    r = db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, id BIGINT);")
    assert not r["ok"] and r["error"]["code"] == "syntax_error", r
    r = db.send("SELECT id FROM t;")
    assert not r["ok"] and r["error"]["code"] == "table_not_found", r
    db.stop()
    print("  ok")


def test_multi_order_heap():
    print("== multi-column ORDER BY + LIMIT heap correctness")
    d = tempfile.mkdtemp(prefix="mvdb-mo-")
    db = Db(d)
    db.start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, a BIGINT, b BIGINT);")["ok"]
    rows = ",".join("(%d,%d,%d)" % (i, i % 3, i % 5) for i in range(1, 31))
    assert db.send("INSERT INTO t VALUES %s;" % rows)["affected_rows"] == 30
    r = db.send("SELECT id FROM t ORDER BY a, b DESC LIMIT 5;")
    got = [x[0] for x in r["rows"]]
    # a asc, then b desc, tie -> id asc
    want = [9, 24, 3, 18, 12]
    assert got == want, (got, want)
    r = db.send("SELECT id FROM t ORDER BY b DESC, a ASC LIMIT 4;")
    got = [x[0] for x in r["rows"]]
    assert got == [9, 24, 4, 19], got
    db.stop()
    print("  ok")


def test_big_delete_update_atomicity_and_recovery():
    print("== large DELETE/UPDATE: streaming, flush, crash atomicity")
    d = tempfile.mkdtemp(prefix="mvdb-big-")
    db = Db(d)
    db.start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v BIGINT, s VARCHAR(16), e VECTOR(2));")["ok"]
    n = 60000
    batch = 1000
    for start in range(1, n + 1, batch):
        end = min(start + batch, n + 1)
        rows = ",".join("(%d,%d,'r%d','[%d,0]')" % (i, i, i, i) for i in range(start, end))
        assert db.send("INSERT INTO t VALUES %s;" % rows)["affected_rows"] == end - start
    # UPDATE half the rows (forces the WAL-readback apply path + memtable flush)
    r = db.send("UPDATE t SET v = v + 0 WHERE id <= 30000;")
    # v+0 is unsupported arithmetic; use a real update instead
    assert not r["ok"], r
    r = db.send("UPDATE t SET v = -1 WHERE id <= 30000;")
    assert r["ok"] and r["affected_rows"] == 30000, r
    r = db.send("SELECT COUNT(id) FROM t WHERE v = -1;")
    assert not r["ok"], r  # aggregates unsupported
    r = db.send("SELECT id FROM t WHERE v = -1 ORDER BY id LIMIT 3;")
    assert [x[0] for x in r["rows"]] == [1, 2, 3]
    # DELETE everything except a few rows
    r = db.send("DELETE FROM t WHERE id > 10;")
    assert r["ok"] and r["affected_rows"] == n - 10, r
    r = db.send("SELECT id FROM t ORDER BY id;")
    assert [x[0] for x in r["rows"]] == list(range(1, 11))
    # restart: state persists
    db.stop()
    db.start()
    r = db.send("SELECT id FROM t ORDER BY id LIMIT 3;")
    assert [x[0] for x in r["rows"]] == [1, 2, 3]
    # kill -9 mid big DELETE: all-or-nothing after recovery
    db.seq += 1
    db.proc.stdin.write(json.dumps(
        {"id": db.seq, "op": "execute", "sql": "DELETE FROM t;",
         "search_mode": "exact"}) + "\n")
    db.proc.stdin.flush()
    time.sleep(0.02)
    db.kill9()
    db.start()
    r = db.send("SELECT id FROM t ORDER BY id;")
    got = [x[0] for x in r["rows"]]
    assert got == list(range(1, 11)) or got == [], got
    db.stop()
    print("  ok")


def test_sigterm_shutdown():
    print("== SIGTERM (stdin still open): graceful rollback + persist + exit 0")
    d = tempfile.mkdtemp(prefix="mvdb-sig-")
    db = Db(d)
    db.start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(16));")["ok"]
    assert db.send("INSERT INTO t VALUES (1,'a'),(2,'b');")["affected_rows"] == 2
    assert db.send("BEGIN;")["ok"]
    assert db.send("DELETE FROM t WHERE id = 1;")["affected_rows"] == 1
    # SIGTERM with stdin open: rollback txn, flush, exit 0
    db.proc.send_signal(signal.SIGTERM)
    try:
        rc = db.proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        db.proc.kill()
        raise AssertionError("did not exit after SIGTERM")
    assert rc == 0, rc
    db.start()
    r = db.send("SELECT id FROM t ORDER BY id;")
    assert [x[0] for x in r["rows"]] == [1, 2], r
    db.stop()
    print("  ok")


def test_edge_values():
    print("== BIGINT extremes / string escapes / column-list INSERT / vector select")
    d = tempfile.mkdtemp(prefix="mvdb-edge-")
    db = Db(d)
    db.start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v BIGINT, s VARCHAR(32), e VECTOR(2));")["ok"]
    r = db.send("INSERT INTO t VALUES (9223372036854775807, -9223372036854775808, 'x', '[1,2]'), (-1, 0, 'quote''and\\\\slash', '[0.5,-0.25]');")
    assert r["ok"] and r["affected_rows"] == 2, r
    r = db.send("SELECT id, v FROM t ORDER BY id;")
    assert r["rows"] == [[-1, 0], [9223372036854775807, -9223372036854775808]], r
    r = db.send("SELECT s FROM t WHERE id = -1;")
    assert r["rows"] == [["quote'and\\slash"]], r
    # column-list insert (permuted order)
    r = db.send("INSERT INTO t (v, id, s, e) VALUES (7, 8, 'perm', '[3,4]');")
    assert r["ok"] and r["affected_rows"] == 1, r
    r = db.send("SELECT id, v, s FROM t WHERE id = 8;")
    assert r["rows"] == [[8, 7, "perm"]], r
    # SELECT * including the vector column (serialized as a string)
    r = db.send("SELECT * FROM t WHERE id = 8;")
    assert r["columns"] == ["id", "v", "s", "e"], r
    assert r["rows"][0][0:3] == [8, 7, "perm"], r
    assert isinstance(r["rows"][0][3], str) and r["rows"][0][3].startswith("[3,4"), r
    # duplicate pk via column list
    r = db.send("INSERT INTO t (id, v, s, e) VALUES (8, 1, 'dup', '[0,0]');")
    assert not r["ok"] and r["error"]["code"] == "duplicate_key", r
    # huge literal overflow
    r = db.send("INSERT INTO t VALUES (9223372036854775808, 0, 'x', '[1,1]');")
    assert not r["ok"] and r["error"]["code"] == "value_overflow", r
    # scientific notation vectors
    r = db.send("INSERT INTO t VALUES (9, 0, 'x', '[1e-3, -2.5E2]');")
    assert r["ok"], r
    r = db.send("SELECT e FROM t WHERE id = 9;")
    assert r["rows"][0][0] == "[0.001,-250]", r
    # ORDER BY varchar = code point order
    r = db.send("INSERT INTO t VALUES (10, 0, 'a', '[0,0]'), (11, 0, 'b', '[0,0]'), (12, 0, 'A', '[0,0]');")
    assert r["affected_rows"] == 3, r
    r = db.send("SELECT id FROM t WHERE id >= 10 AND id <= 12 ORDER BY s;")
    assert [x[0] for x in r["rows"]] == [12, 10, 11], r  # 'A'(65) < 'a'(97) < 'b'(98)
    db.stop()
    print("  ok")


def test_long_statement():
    print("== very long single statement (100K rows in one INSERT)")
    d = tempfile.mkdtemp(prefix="mvdb-long-")
    db = Db(d)
    db.start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(4));")["ok"]
    n = 100000
    rows = ",".join("(%d,'[%d,0,0,0]')" % (i, i) for i in range(1, n + 1))
    stmt = "INSERT INTO t VALUES %s;" % rows
    assert len(stmt) > 1_500_000
    r = db.send(stmt)
    assert r["ok"] and r["affected_rows"] == n, str(r)[:200]
    r = db.send("SELECT id FROM t ORDER BY id LIMIT 3;")
    assert [x[0] for x in r["rows"]] == [1, 2, 3]
    db.stop()
    print("  ok")


def test_tombstone_in_memtable_over_runs():
    print("== tombstone in memtable must hide rows already flushed to runs")
    d = tempfile.mkdtemp(prefix="mvdb-ts-")
    db = Db(d)
    db.start()
    assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(16));")["ok"]
    n = 60000  # exceeds the memtable threshold -> rows flushed to runs
    for start in range(1, n + 1, 1000):
        end = min(start + 1000, n + 1)
        rows = ",".join("(%d,'r%d')" % (i, i) for i in range(start, end))
        assert db.send("INSERT INTO t VALUES %s;" % rows)["affected_rows"] == end - start
    # rows 1..n now live in runs; the deletes below land in the memtable
    assert db.send("DELETE FROM t WHERE id = 2;")["affected_rows"] == 1
    assert db.send("DELETE FROM t WHERE id <= 5;")["affected_rows"] == 4  # 1,3,4,5
    assert db.send("UPDATE t SET v = 'changed' WHERE id = 10;")["affected_rows"] == 1
    r = db.send("SELECT id FROM t ORDER BY id LIMIT 5;")
    assert [x[0] for x in r["rows"]] == [6, 7, 8, 9, 10], r
    r = db.send("SELECT id FROM t WHERE id IN (1,2,5);")
    assert not r["ok"], r  # IN unsupported
    r = db.send("SELECT id FROM t WHERE id = 2 OR id = 5;")
    assert r["rows"] == [], r
    r = db.send("SELECT v FROM t WHERE id = 10;")
    assert r["rows"] == [["changed"]], r
    db.stop()
    db.start()
    r = db.send("SELECT id FROM t ORDER BY id LIMIT 5;")
    assert [x[0] for x in r["rows"]] == [6, 7, 8, 9, 10], r
    db.stop()
    print("  ok")


def main():
    test_committed_survives_kill9()
    test_kill9_mid_autocommit_batch()
    test_invalid_vectors()
    test_partial_wal_tail()
    test_ann_recall_small()
    test_index_maintenance_txn()
    test_where_unknown_column()
    test_update_unchanged_not_counted()
    test_duplicate_column_create()
    test_multi_order_heap()
    test_big_delete_update_atomicity_and_recovery()
    test_sigterm_shutdown()
    test_edge_values()
    test_long_statement()
    test_tombstone_in_memtable_over_runs()
    print("ALL ROBUSTNESS TESTS PASSED")


if __name__ == "__main__":
    main()
