#!/usr/bin/env python3
"""Parser fuzz: random/malformed SQL must always produce a JSON error
response and never crash the process."""
import json
import os
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from transport import Db  # noqa: E402

BINARY = sys.argv[1] if len(sys.argv) > 1 else "./minivectordb"
ITER = int(sys.argv[2]) if len(sys.argv) > 2 else 1500
rng = random.Random(1234)

TOKENS = ["SELECT", "INSERT", "CREATE", "TABLE", "FROM", "WHERE", "ORDER", "BY",
          "LIMIT", "AND", "OR", "NOT", "VALUES", "INTO", "UPDATE", "SET",
          "DELETE", "BEGIN", "COMMIT", "ROLLBACK", "VECTOR", "VARCHAR", "BIGINT",
          "PRIMARY", "KEY", "INDEX", "L2_DISTANCE", "AS", "DESC", "ASC",
          "(", ")", ",", ";", "=", "<", ">", "<=", ">=", "!=", "*", "'", '"',
          "[", "]", "-", "+", ".", "_", " ", "\t",
          "0", "1", "-1", "99999999999999999999999999", "1e999", "NaN", "Infinity",
          "'unterminated", "'\x00'", "''", "'''", "'\\\\'", "中文", "émoji🚀",
          "id", "docs", "embedding", "content", "category", "t", "--comment",
          "L2_DISTANCE(e, '[1,2]')", "VECTOR(2)", "VARCHAR(16)", "BIGINT PRIMARY KEY",
          "\\", "\x00", "\xff\xfe", "..", ",,", "))", "((", "''''"]


def gen_line():
    n = rng.randint(1, 30)
    return " ".join(rng.choice(TOKENS) for _ in range(n))


db = Db([BINARY, "--data-dir", tempfile.mkdtemp(prefix="mvdb-pfuzz-")], None)
db.start()

for i in range(ITER):
    line = gen_line()
    if not line.strip():
        continue  # blank lines carry no statement (题包: 每个非空行是一条 SQL)
    sys.stderr.write("SEND %d: %r\n" % (i, line[:100]))
    sys.stderr.flush()
    r = db.send(line)
    # any response shape is fine as long as it's a valid JSON object
    assert isinstance(r, dict) and ("ok" in r), (line[:80], r)
    if i % 100 == 0:
        print("fuzz %d/%d ok (last: %r)" % (i, ITER, line[:60]))

# the process must still work correctly afterwards
assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v BIGINT, e VECTOR(2));")["ok"]
assert db.send("INSERT INTO t VALUES (1, 10, '[1,2]'), (2, 20, '[3,4]');")["affected_rows"] == 2
r = db.send("SELECT id, L2_DISTANCE(e, '[0,0]') AS d FROM t ORDER BY d LIMIT 1;")
assert r["ok"] and r["rows"] and r["rows"][0][0] == 1 and abs(r["rows"][0][1] - 2.2360679775) < 1e-6, r
db.shutdown()
print("PARSER FUZZ OK (%d iterations)" % ITER)
