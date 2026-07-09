#!/usr/bin/env python3
import socket, time, subprocess, os, shutil, threading, random, collections

DBDIR = "/home/nanfeng/桌面/shujvk/rmdb"
RMDB = f"{DBDIR}/build/bin/rmdb"
DB = "test_wa"
DATA = f"{DBDIR}/src/test/performance_test/table_data_gen"

def conn():
    for _ in range(30):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(5)
            s.connect(('127.0.0.1', 8765))
            return s
        except:
            time.sleep(0.5)
    raise Exception("Cannot connect")

def sql(s, q, t=30):
    if q.strip() and not q.strip().endswith(';'): q += ';'
    s.settimeout(t)
    s.sendall((q + '\0').encode())
    b = []
    while True:
        try:
            d = s.recv(65536)
        except:
            return 'TIMEOUT'
        if not d: break
        b.append(d)
        if b'\0' in d: break
    return b''.join(b).decode(errors='replace').rstrip('\0')

def fi(r):
    for l in r.splitlines():
        if l.startswith('|'):
            for v in [x.strip() for x in l.strip('|').split('|')]:
                if v and v.lstrip('-').isdigit(): return int(v)
    return None

os.chdir(DBDIR)
dp = os.path.join(DBDIR, DB)
if os.path.exists(dp): shutil.rmtree(dp)
rmdb = subprocess.Popen([RMDB, DB], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

# Setup connection - NO SI isolation level
s = conn()
sql(s, 'set output_file off')

tables = [
    'create table warehouse (w_id int, w_name char(10), w_street_1 char(20), w_street_2 char(20), w_city char(20), w_state char(2), w_zip char(9), w_tax float, w_ytd float)',
    'create table district (d_id int, d_w_id int, d_name char(10), d_street_1 char(20), d_street_2 char(20), d_city char(20), d_state char(2), d_zip char(9), d_tax float, d_ytd float, d_next_o_id int)',
    'create table customer (c_id int, c_d_id int, c_w_id int, c_first char(16), c_middle char(2), c_last char(16), c_street_1 char(20), c_street_2 char(20), c_city char(20), c_state char(2), c_zip char(9), c_phone char(16), c_since char(30), c_credit char(2), c_credit_lim int, c_discount float, c_balance float, c_ytd_payment float, c_payment_cnt int, c_delivery_cnt int, c_data char(50))',
    'create table history (h_c_id int, h_c_d_id int, h_c_w_id int, h_d_id int, h_w_id int, h_date char(30), h_amount float, h_data char(24))',
    'create table new_orders (no_o_id int, no_d_id int, no_w_id int)',
    'create table orders (o_id int, o_d_id int, o_w_id int, o_c_id int, o_entry_d char(30), o_carrier_id int, o_ol_cnt int, o_all_local int)',
    'create table order_line (ol_o_id int, ol_d_id int, ol_w_id int, ol_number int, ol_i_id int, ol_supply_w_id int, ol_delivery_d char(30), ol_quantity float, ol_amount float, ol_dist_info char(24))',
    'create table item (i_id int, i_im_id int, i_name char(24), i_price float, i_data char(50))',
    'create table stock (s_i_id int, s_w_id int, s_quantity float, s_dist_01 char(24), s_dist_02 char(24), s_dist_03 char(24), s_dist_04 char(24), s_dist_05 char(24), s_dist_06 char(24), s_dist_07 char(24), s_dist_08 char(24), s_dist_09 char(24), s_dist_10 char(24), s_ytd float, s_order_cnt int, s_remote_cnt int, s_data char(50))',
]
for q in tables:
    r = sql(s, q, 10)
    if 'error' in r.lower(): print(f"CREATE FAIL: {r[:80]}")

for t in ['warehouse','district','customer','item','stock']:
    r = sql(s, f'load {DATA}/{t}.csv into {t}', 600)
    if 'error' in r.lower(): print(f"LOAD FAIL {t}: {r[:80]}")

for q in [
    'create index warehouse(w_id)',
    'create index district(d_w_id, d_id)',
    'create index customer(c_w_id, c_d_id, c_id)',
    'create index item(i_id)',
    'create index stock(s_w_id, s_i_id)',
    'create index orders(o_w_id, o_d_id, o_id)',
    'create index new_orders(no_w_id, no_d_id, no_o_id)',
    'create index order_line(ol_w_id, ol_d_id, ol_o_id, ol_number)',
]:
    r = sql(s, q, 300)
    if 'error' in r.lower(): print(f"INDEX FAIL: {r[:80]}")

for t in ['district','customer','stock','item','warehouse']:
    cnt = fi(sql(s, f'select count(*) from {t}'))
    print(f"  {t}: {cnt} rows")

s.close()
time.sleep(1)

# 16 threads, 3 districts (online config), 30s
print("\nRunning 16 threads x 3 districts x 30s...")
ND = 3
stop = threading.Event()
stats = collections.Counter()
lock = threading.Lock()

def worker(tid):
    try:
        ws = conn()
        sql(ws, 'set output_file off')
        sql(ws, 'set transaction isolation level SNAPSHOT ISOLATION')
        rng = random.Random(tid)
        while not stop.is_set():
            w=1; d=rng.randint(1,ND); c=rng.randint(1,3000)
            sql(ws, 'begin')
            try:
                sql(ws, f'select c_discount from customer where c_w_id={w} and c_d_id={d} and c_id={c}')
                r=sql(ws, f'select d_next_o_id from district where d_id={d} and d_w_id={w}')
                oid=fi(r)
                if oid is None: sql(ws,'abort'); lock.__enter__(); stats['abort']+=1; lock.__exit__(None,None,None); continue
                r=sql(ws, f'update district set d_next_o_id={oid+1} where d_id={d} and d_w_id={w}')
                if 'abort' in r.lower(): sql(ws,'abort'); lock.__enter__(); stats['abort']+=1; lock.__exit__(None,None,None); continue
                r=sql(ws, f"insert into orders values ({oid},{d},{w},{c},'2026-06-30 00:00:00',0,5,1)")
                if 'abort' in r.lower(): sql(ws,'abort'); lock.__enter__(); stats['abort']+=1; lock.__exit__(None,None,None); continue
                r=sql(ws, f'insert into new_orders values ({oid},{d},{w})')
                if 'abort' in r.lower(): sql(ws,'abort'); lock.__enter__(); stats['abort']+=1; lock.__exit__(None,None,None); continue
                fail=False
                for n in range(1,6):
                    iid=rng.randint(1,100000)
                    r=sql(ws, f'update stock set s_quantity=45 where s_i_id={iid} and s_w_id={w}')
                    if 'abort' in r.lower(): fail=True; break
                if fail: sql(ws,'abort'); lock.__enter__(); stats['abort']+=1; lock.__exit__(None,None,None); continue
                r=sql(ws, 'commit')
                if 'abort' in r.lower(): lock.__enter__(); stats['abort']+=1; lock.__exit__(None,None,None)
                else: lock.__enter__(); stats['commit']+=1; lock.__exit__(None,None,None)
            except:
                with lock: stats['error']+=1
                try: sql(ws,'abort')
                except: pass
        ws.close()
    except: pass

threads = [threading.Thread(target=worker, args=(i,)) for i in range(16)]
for t in threads: t.start()
time.sleep(3); stats.clear()
time.sleep(30)
stop.set()
for t in threads: t.join(timeout=5)

total = stats['commit']+stats['abort']
ar = stats['abort']/total*100 if total else 0
tpmC = stats['commit']/(30/60)
print(f"Commits: {stats['commit']}, Aborts: {stats['abort']}, Abort%: {ar:.1f}%, tpmC: {tpmC:.0f}")

# Check if rmdb still alive
if rmdb.poll() is not None:
    stderr = rmdb.stderr.read().decode(errors='replace')
    print(f"\n!!! rmdb CRASHED (exit code {rmdb.returncode}) !!!")
    print(f"stderr: {stderr[-2000:]}")
    rmdb.wait(timeout=2)
    shutil.rmtree(dp, ignore_errors=True)
    exit(1)

# Consistency check
print("\n=== Consistency Check ===")
cs = conn()
sql(cs, 'set output_file off')

for d in range(1, ND+1):
    dn = fi(sql(cs, f'select d_next_o_id from district where d_id={d} and d_w_id=1'))
    mx = fi(sql(cs, f'select max(o_id) from orders where o_d_id={d} and o_w_id=1'))
    oc = fi(sql(cs, f'select count(*) from orders where o_d_id={d} and o_w_id=1'))
    nc = fi(sql(cs, f'select count(*) from new_orders where no_d_id={d} and no_w_id=1'))
    ok = '✓' if dn and mx and dn > mx else 'INCONSISTENT!'
    print(f"  D{d}: d_next={dn}, max_oid={mx}, orders={oc}, new_orders={nc} {ok}")

ol = fi(sql(cs, 'select count(*) from order_line'))
ot = fi(sql(cs, 'select count(*) from orders'))
print(f"\n  order_lines={ol}, orders={ot}")

cs.close()
rmdb.terminate()
rmdb.wait(timeout=5)
shutil.rmtree(dp, ignore_errors=True)
print("\nDone!")
