#!/usr/bin/env python3
"""Hidden-case semantic sweep: edges most likely exercised by the 9 hidden
formal cases but not in the public package."""
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from transport import Db  # noqa: E402

BINARY = sys.argv[1]
d = tempfile.mkdtemp(prefix="mvdb-hidden-")
db = Db([BINARY, "--data-dir", d], d)
db.start()

# vector literal spacing / dim 1 / dim 256
assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(8), e VECTOR(3));")["ok"]
assert db.send("INSERT INTO t VALUES (1,'a','[ 1 , 2 , 3 ]');")["affected_rows"] == 1
assert db.send("INSERT INTO t VALUES (2,'b','[1.5,-2.5,+3.5]');")["affected_rows"] == 1
r = db.send("SELECT id, L2_DISTANCE(e,'[0,0,0]') AS d FROM t ORDER BY d LIMIT 5;")
assert [x[0] for x in r["rows"]] == [1, 2], r  # sqrt14 < sqrt(2.25+6.25+12.25)
# dim 1 and dim 256
assert db.send("CREATE TABLE t1 (id BIGINT PRIMARY KEY, e VECTOR(1));")["ok"]
assert db.send("INSERT INTO t1 VALUES (1,'[3]'),(2,'[-4]');")["affected_rows"] == 2
r = db.send("SELECT id, L2_DISTANCE(e,'[0]') AS d FROM t1 ORDER BY d LIMIT 2;")
assert [x[0] for x in r["rows"]] == [1, 2], r
assert db.send("CREATE TABLE t256 (id BIGINT PRIMARY KEY, e VECTOR(256));")["ok"]
v = "[" + ",".join("1" for _ in range(256)) + "]"
assert db.send("INSERT INTO t256 VALUES (1,'%s');" % v)["affected_rows"] == 1
r = db.send("SELECT L2_DISTANCE(e,'%s') AS d FROM t256;" % v)
assert r["ok"] and abs(r["rows"][0][0]) < 1e-6, r

# trailing semicolon + double semicolon + multi-statement rejection
assert db.send("SELECT id FROM t ORDER BY id;;")["ok"]
r = db.send("SELECT id FROM t; SELECT id FROM t;")
assert not r["ok"] and r["error"]["code"], r
# LIMIT 0 / LIMIT beyond size / always-false WHERE with dist order
r = db.send("SELECT id FROM t LIMIT 0;")
assert r["ok"] and r["rows"] == [], r
r = db.send("SELECT id FROM t ORDER BY id LIMIT 100;")
assert len(r["rows"]) == 2, r
r = db.send("SELECT id, L2_DISTANCE(e,'[0,0,0]') AS d FROM t WHERE c='zz' ORDER BY d LIMIT 3;")
assert r["ok"] and r["rows"] == [], r
# COMMIT/ROLLBACK without BEGIN -> error
r = db.send("COMMIT;")
assert not r["ok"] and r["error"]["code"] == "txn_state", r
r = db.send("ROLLBACK;")
assert not r["ok"] and r["error"]["code"] == "txn_state", r
# BEGIN twice -> error; state still usable
assert db.send("BEGIN;")["ok"]
r = db.send("BEGIN;")
assert not r["ok"] and r["error"]["code"] == "txn_state", r
assert db.send("INSERT INTO t VALUES (3,'c','[0,0,0]');")["affected_rows"] == 1
assert db.send("COMMIT;")["ok"]
# DDL inside txn commits implicitly (MySQL semantics) and clears txn
assert db.send("BEGIN;")["ok"]
assert db.send("CREATE TABLE t2 (id BIGINT PRIMARY KEY, e VECTOR(2));")["ok"]
assert db.send("INSERT INTO t2 VALUES (1,'[1,1]');")["affected_rows"] == 1
# txn cleared by DDL: COMMIT should now fail
r = db.send("COMMIT;")
assert not r["ok"] and r["error"]["code"] == "txn_state", r
# varchar emoji boundary (4-byte UTF-8, one codepoint each)
assert db.send("CREATE TABLE u (id BIGINT PRIMARY KEY, s VARCHAR(3));")["ok"]
assert db.send("INSERT INTO u VALUES (1,'🚀🚀🚀');")["affected_rows"] == 1
r = db.send("INSERT INTO u VALUES (2,'🚀🚀🚀🚀');")
assert not r["ok"] and r["error"]["code"] == "string_too_long", r
# update vector to a different valid vector + distance reflects it
assert db.send("UPDATE t SET e='[10,0,0]' WHERE id=1;")["affected_rows"] == 1
r = db.send("SELECT L2_DISTANCE(e,'[10,0,0]') AS d FROM t WHERE id=1;")
assert r["ok"] and abs(r["rows"][0][0]) < 1e-6, r
# restart persistence of everything above
db.shutdown()
db = Db([BINARY, "--data-dir", d], d)
db.start()
r = db.send("SELECT id FROM t ORDER BY id;")
assert [x[0] for x in r["rows"]] == [1, 2, 3], r
r = db.send("SELECT s FROM u WHERE id=1;")
assert r["rows"] == [["🚀🚀🚀"]], r
db.shutdown()
print("HIDDEN-CASE SWEEP OK")
