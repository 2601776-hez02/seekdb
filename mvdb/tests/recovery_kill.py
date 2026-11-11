#!/usr/bin/env python3
"""Kill during recovery: the official requirement 'recovery interrupted by
another crash must still recover' (恢复途中再次退出仍能恢复)."""
import json
import os
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from transport import Db  # noqa: E402

BINARY = sys.argv[1]
ENV = dict(os.environ)
ENV["MVDB_MEMTABLE_BYTES"] = "262144"          # 256 KiB: flush during replay
ENV["MVDB_WAL_CHECKPOINT_BYTES"] = "262144"    # 256 KiB: checkpoints often

D = tempfile.mkdtemp(prefix="mvdb-reckill-")


def start_quiet():
    db = Db([BINARY, "--data-dir", D], D, env=ENV)
    db.start()
    return db


# phase 1: build up 150K rows committed across many statements
db = start_quiet()
assert db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8), e VECTOR(2));")["ok"]
for b in range(0, 150000, 1000):
    rows = ",".join("(%d,'r%d','[%d,0]')" % (i, i, i) for i in range(b + 1, b + 1001))
    r = db.send("INSERT INTO t VALUES %s;" % rows)
    assert r["ok"] and r["affected_rows"] == 1000, r
print("phase1: 150K rows committed")
db.kill9()

# phase 2: repeatedly kill during recovery/rebuild (READY not yet printed)
for attempt in range(5):
    proc = subprocess.Popen([BINARY, "--data-dir", D],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, universal_newlines=True,
                            bufsize=1, env=ENV)
    time.sleep(0.4 + 0.3 * attempt)   # land mid-recovery at varying depths
    proc.kill()
    proc.wait()
    print("kill-during-recovery attempt %d done" % (attempt + 1))

# phase 3: final recovery must yield the exact committed state
db = start_quiet()
r = db.send("SELECT id FROM t ORDER BY id DESC LIMIT 1;")
assert r["ok"] and r["rows"][0][0] == 150000, r
r = db.send("SELECT id FROM t ORDER BY id LIMIT 3;")
assert [x[0] for x in r["rows"]] == [1, 2, 3], r
r = db.send("SELECT id, L2_DISTANCE(e,'[0,0]') AS d FROM t ORDER BY d LIMIT 1;")
assert r["ok"] and r["rows"][0][0] == 1, r
db.shutdown()
print("KILL-DURING-RECOVERY OK")
