"""Oracle for visible-key uniqueness across memtable, overlay and run tombstones."""
import json,os,subprocess,sys,tempfile
binary=sys.argv[1]
class DB:
    def __init__(self,path,flush=False):
        env=os.environ.copy()
        if flush: env['MVDB_MEMTABLE_BYTES']='4096'
        self.p=subprocess.Popen([binary,'--data-dir',path],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,universal_newlines=True,env=env)
        assert json.loads(self.p.stdout.readline())=={'type':'ready'}
        self.i=0
    def execute(self,sql,ok=True):
        self.i+=1
        self.p.stdin.write(json.dumps({'id':self.i,'op':'execute','sql':sql})+'\n');self.p.stdin.flush()
        r=json.loads(self.p.stdout.readline())
        assert r['ok']==ok,(sql,r)
        return r
    def stop(self):
        self.p.stdin.write(json.dumps({'id':self.i+1,'op':'shutdown'})+'\n');self.p.stdin.flush()
        assert json.loads(self.p.stdout.readline())['ok']
        self.p.stdin.close();assert self.p.wait(timeout=30)==0
failures=[]
for mode in ['autocommit','transaction','rollback','run']:
    path=tempfile.mkdtemp(prefix='reinsert-'+mode+'-')
    c=DB(path,mode=='run')
    try:
        c.execute('CREATE TABLE t (id BIGINT PRIMARY KEY,c VARCHAR(8192));')
        pad='x'*5000 if mode=='run' else 'old'
        c.execute("INSERT INTO t VALUES (1,'%s');"%pad)
        if mode in ('transaction','rollback'):c.execute('BEGIN;')
        c.execute('DELETE FROM t WHERE id=1;')
        rows=c.execute('SELECT id,c FROM t;')['rows']
        assert rows==[],('after delete',str(rows)[:160])
        if mode=='run':
            c.stop();c=DB(path,True)
            rows=c.execute('SELECT id,c FROM t;')['rows']
            assert rows==[],('after restart',str(rows)[:160])
        c.execute("INSERT INTO t VALUES (1,'new');")
        assert c.execute('SELECT id,c FROM t;')['rows']==[[1,'new']]
        r=c.execute("INSERT INTO t VALUES (1,'dup');",False)
        assert r['error']['code']=='duplicate_key'
        if mode=='transaction':c.execute('COMMIT;')
        if mode=='rollback':c.execute('ROLLBACK;')
        assert c.execute('SELECT id,c FROM t;')['rows']==[[1,'old' if mode=='rollback' else 'new']]
        c.stop();c=DB(path,mode=='run')
        assert c.execute('SELECT id,c FROM t;')['rows']==[[1,'old' if mode=='rollback' else 'new']]
        print(mode,'PASS',flush=True)
    except AssertionError as e:
        failures.append((mode,str(e)))
        print(mode,'FAIL',str(e),flush=True)
    finally:c.stop()
assert not failures,failures
