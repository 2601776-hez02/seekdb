#!/usr/bin/env python3
"""Vector projection round-trip: write -> commit -> read back -> restart ->
read back again.

Checks the protocol contract (STDIO_PROTOCOL 3): vector projections are JSON
arrays of finite numbers, and the serialized text must parse back to the exact
float32 value stored. Uses mixed magnitudes and non-integer values so the
sample does not collapse to the [3,4] special case."""
import math
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from transport import Db as _Base  # noqa: E402

BINARY = sys.argv[1] if len(sys.argv) > 1 else "./minivectordb"


def f32(x):
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


def f32hex(x):
    return struct.pack("<f", f32(x)).hex()


class Db(_Base):
    def __init__(self, data_dir):
        super().__init__([BINARY, "--data-dir", data_dir], data_dir)


VECTORS = [
    [0.25, -0.5, 0.125, -1.75, 3.5, -7.25, 0.0625, -0.03125],
    [1e-30, -1e-30, 1e30, -1e30, 16777216, -16777216, 0.1, -0.1],
    [3.14159, 2.71828, 1.41421, 0.69315, -0.57722, 1.61803, -2.23607, 0.001],
]
UPDATED = [-0.1, 0.2, -0.3, 0.4, 1e-6, -1e-6, 12345.678, -7.5]
WIDE = [round(-0.5 + i * 0.0078125, 6) for i in range(128)]


def lit(v):
    return "[" + ",".join(repr(float(x)) for x in v) + "]"


def check_vec(resp, want, name):
    assert resp["ok"], (name, resp)
    rows = resp["rows"]
    assert len(rows) == 1, (name, "rows", len(rows))
    got = rows[0][0]
    assert isinstance(got, list), (name, "not a JSON array:", type(got).__name__)
    assert len(got) == len(want), (name, "dimension", len(got), "!=", len(want))
    for i, (g, w) in enumerate(zip(got, want)):
        assert isinstance(g, (int, float)) and not isinstance(g, bool), (name, i, "type", type(g))
        assert math.isfinite(g), (name, i, "non-finite", g)
        assert f32hex(g) == f32hex(w), (name, i, "float32 round-trip", g, w)
    return got


def main():
    root = tempfile.mkdtemp(prefix="mvdb-vround-")
    db = Db(root)
    db.start()
    assert db.send("CREATE TABLE p (id BIGINT PRIMARY KEY, c VARCHAR(16), e VECTOR(8));")["ok"]
    assert db.send("CREATE TABLE q (id BIGINT PRIMARY KEY, e VECTOR(128));")["ok"]

    # explicit transaction: write -> COMMIT -> read back
    assert db.send("BEGIN;")["ok"]
    assert db.send("INSERT INTO p VALUES (1, 'a', '%s');" % lit(VECTORS[0]))["ok"]
    assert db.send("COMMIT;")["ok"]

    # autocommit rows
    for i in (2, 3):
        assert db.send("INSERT INTO p VALUES (%d, 'x%d', '%s');" % (i, i, lit(VECTORS[i - 1])))["ok"]
    assert db.send("INSERT INTO q VALUES (1, '%s');" % lit(WIDE))["ok"]

    # direct projection / alias projection / SELECT *
    r = db.send("SELECT e FROM p WHERE id = 1;")
    assert r["columns"] == ["e"], r
    check_vec(r, VECTORS[0], "direct id=1")
    r = db.send("SELECT e AS v FROM p WHERE id = 2;")
    assert r["columns"] == ["v"], r
    check_vec(r, VECTORS[1], "alias id=2")
    r = db.send("SELECT * FROM p WHERE id = 3;")
    assert r["columns"] == ["id", "c", "e"], r
    assert r["rows"][0][0:2] == [3, "x3"], r
    check_vec({"ok": True, "rows": [[r["rows"][0][2]]]}, VECTORS[2], "select-star id=3")
    r = db.send("SELECT e FROM q WHERE id = 1;")
    check_vec(r, WIDE, "wide-128")

    # update -> read back
    assert db.send("UPDATE p SET e = '%s' WHERE id = 1;" % lit(UPDATED))["affected_rows"] == 1
    check_vec(db.send("SELECT e FROM p WHERE id = 1;"), UPDATED, "updated id=1")

    # restart -> read back again (data dir reused)
    db.stop()
    db.start()
    check_vec(db.send("SELECT e FROM p WHERE id = 1;"), UPDATED, "after-restart id=1")
    check_vec(db.send("SELECT e FROM p WHERE id = 2;"), VECTORS[1], "after-restart id=2")
    check_vec(db.send("SELECT e FROM p WHERE id = 3;"), VECTORS[2], "after-restart id=3")
    check_vec(db.send("SELECT e FROM q WHERE id = 1;"), WIDE, "after-restart wide-128")

    # ---- large legal magnitudes: emitted distance = double reference ----
    # (a) input must reject float32 overflow / non-finite literals
    assert db.send("CREATE TABLE big (id BIGINT PRIMARY KEY, e VECTOR(4));")["ok"]
    BIG = [(1, [1.0, 0.0, 0.0, 0.0]),
           (2, [1e9, 0.0, 0.0, 0.0]),
           (3, [1e19, 0.0, 0.0, 0.0]),
           (4, [1e30, 0.0, 0.0, 0.0])]
    for pk, v in BIG:
        assert db.send("INSERT INTO big VALUES (%d, '%s');" % (pk, lit(v)))["ok"]
    for bad in ("[1e39,0,0,0]", "[nan,0,0,0]", "[inf,0,0,0]"):
        r = db.send("INSERT INTO big VALUES (9, '%s');" % bad)
        assert not r["ok"] and r["error"]["code"] == "invalid_vector", (bad, r)
    # (b) distance values and order must match an independent double reference
    r = db.send("SELECT id, L2_DISTANCE(e,'[0,0,0,0]') AS d FROM big ORDER BY d LIMIT 10;")
    got = [(int(row[0]), float(row[1])) for row in r["rows"]]
    ref = sorted((pk, math.sqrt(sum(float(a) * float(a) for a in v))) for pk, v in BIG)
    assert [g[0] for g in got] == [x[0] for x in ref], (got, ref)
    for (gpk, gd), (rpk, rd) in zip(got, ref):
        assert gpk == rpk, (got, ref)
        assert abs(gd - rd) <= 1e-6 * max(1.0, abs(rd)), (gpk, gd, rd)
    db.stop()
    print("VECTOR ROUND-TRIP OK (direct/alias/select-star, update, restart, 128d)")


if __name__ == "__main__":
    main()
