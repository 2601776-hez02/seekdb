"""Behavior oracle and paired performance gate for serial scan caching."""
import json, random, statistics, subprocess, sys, tempfile, time
BASE = sys.argv[1]
CAND = sys.argv[1] if len(sys.argv) > 1 else BASE

class DB:
    def __init__(self, binary, root):
        self.p = subprocess.Popen([binary, '--data-dir', root], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, universal_newlines=True)
        assert json.loads(self.p.stdout.readline()) == {'type':'ready'}
        self.seq = 0
    def sql(self, sql):
        self.seq += 1
        self.p.stdin.write(json.dumps({'id':self.seq,'op':'execute','sql':sql,'search_mode':'exact'})+'\n')
        self.p.stdin.flush()
        r = json.loads(self.p.stdout.readline())
        assert r['ok'], r
        return r
    def close(self):
        self.p.stdin.write(json.dumps({'id':self.seq+1,'op':'shutdown'})+'\n')
        self.p.stdin.flush()
        assert json.loads(self.p.stdout.readline())['ok']
        self.p.stdin.close()
        assert self.p.wait(timeout=120) == 0

def behavior(binary):
    root = tempfile.mkdtemp(prefix='cache-check-')
    db = DB(binary, root)
    model = {i: float(i) for i in range(1, 201)}
    db.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(1));')
    db.sql('INSERT INTO t VALUES '+','.join("(%d,'[%d]')"%(i,i) for i in model)+';')
    def check():
        # Repeated scans warm the cache before every mutation.
        for _ in range(3):
            rows = db.sql("SELECT id,L2_DISTANCE(e,'[0]') AS d FROM t ORDER BY d LIMIT 10;")['rows']
            assert [r[0] for r in rows] == sorted(model, key=lambda i:(abs(model[i]),i))[:10], rows
    try:
        check()
        db.sql("UPDATE t SET e='[0.125]' WHERE id=199;")
        model[199] = .125
        check()
        saved = dict(model)
        db.sql('BEGIN;')
        db.sql("UPDATE t SET e='[0.0625]' WHERE id=198;")
        model[198] = .0625
        db.sql('DELETE FROM t WHERE id=199;')
        del model[199]
        check()
        db.sql('ROLLBACK;')
        model = saved
        check()
        db.sql('BEGIN;')
        db.sql("UPDATE t SET e='[0.03125]' WHERE id=199;")
        db.sql('COMMIT;')
        model[199] = .03125
        check()
        db.sql('DELETE FROM t WHERE id=1;')
        del model[1]
        db.sql("INSERT INTO t VALUES (201,'[0.015625]');")
        model[201] = .015625
        check()
        db.close()
        db = DB(binary, root)
        check()
    finally:
        db.close()

behavior(BASE)
print("SERIAL SCAN VISIBILITY PASS")
