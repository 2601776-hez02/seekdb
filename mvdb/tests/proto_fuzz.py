#!/usr/bin/env python3
"""Protocol robustness fuzz: random malformed JSONL lines, random bytes, huge
ids, weird search_mode values; the process must never crash and must keep
answering correct SQL afterwards."""
import json
import random
import string
import subprocess
import tempfile
import time

BINARY = "/root/mvdb/minivectordb"
rng = random.Random(101)

d = tempfile.mkdtemp(prefix="mvdb-pfuzz-")
p = subprocess.Popen([BINARY, "--data-dir", d], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                     universal_newlines=True, bufsize=1 << 20)
assert json.loads(p.stdout.readline()) == {"type": "ready"}
seq = [0]
def send_raw(line):
    p.stdin.write(line + "\n")
    p.stdin.flush()
def send(sql, mode="exact"):
    seq[0] += 1
    p.stdin.write(json.dumps({"id": seq[0], "op": "execute", "sql": sql,
                              "search_mode": mode}) + "\n")
    p.stdin.flush()
    out = p.stdout.readline()
    assert out, "no response after fuzz"
    return json.loads(out)

send("CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(8), e VECTOR(16));")
rows = ",".join("(%d,'c%d','[%s]')" % (i, i % 7, ",".join("%.4f" % rng.uniform(-1, 1) for _ in range(16)))
                for i in range(1, 201))
assert send("INSERT INTO t VALUES %s;" % rows)["ok"]

# sanity reference
r0 = send("SELECT id FROM t ORDER BY id LIMIT 5;")
ref = r0["rows"]

alphabet = string.printable + "\x00\x01\x7f"
for i in range(3000):
    kind = rng.randrange(6)
    if kind == 0:
        # random bytes line
        n = rng.randrange(0, 120)
        line = "".join(rng.choice(alphabet) for _ in range(n))
    elif kind == 1:
        # structurally valid JSON with random fields
        line = json.dumps({
            "id": rng.choice([0, 1, "x", None, [1, 2], 10**rng.randrange(0, 25)]),
            "op": rng.choice(["execute", "", None, "EXECUTE", "no_such_op"]),
            "sql": rng.choice(["", "SELECT 1;", "DROP ALL;", "select * from t"]),
            "search_mode": rng.choice(["exact", "ann", "", "EXACT", None]),
            "extra": rng.random(),
        })
    elif kind == 2:
        # truncated / concatenated JSON
        line = json.dumps({"id": 1, "op": "execute", "sql": "SELECT 1;"})[:-rng.randrange(1, 6)]
    elif kind == 3:
        # huge line (should be read and rejected)
        line = '"' * 200000
    elif kind == 4:
        # valid request with a huge id (echo must not crash)
        line = json.dumps({"id": 10**rng.randrange(0, 30), "op": "execute",
                           "sql": "SELECT id FROM t ORDER BY id LIMIT 1;",
                           "search_mode": "exact"})
    else:
        # non-object JSON values
        line = rng.choice(['42', 'null', '[]', '"str"', '{"a":1,}', '{'])

    before = p.poll()
    send_raw(line)
    # response may or may not come (malformed lines are ignored, valid ones
    # respond); read what's available without blocking: use a short poll
    import select
    r, _, _ = select.select([p.stdout], [], [], 0.05)
    if r:
        p.stdout.readline()
    if p.poll() is not None:
        print("PROCESS DIED at fuzz iteration %d (kind %d, line %.80r)" % (i, kind, line))
        raise SystemExit(1)

# after 3000 malformed lines the DB must still answer correctly
r = send("SELECT id FROM t ORDER BY id LIMIT 5;")
assert r["rows"] == ref, (r["rows"], ref)
print("PROTOCOL FUZZ OK (3000 malformed lines, DB alive and correct)")
p.stdin.write(json.dumps({"id": 999, "op": "shutdown"}) + "\n")
p.stdin.flush()
p.stdout.readline()
p.wait(timeout=60)
print("clean shutdown, exit", p.returncode)
