#!/usr/bin/env python3
import subprocess, time, os, shutil, socket, random, collections, threading

DBDIR = "/home/nanfeng/桌面/shujvk/rmdb"
RMDB = f"{DBDIR}/build/bin/rmdb"
DB = "test_timing"
DATA = "../src/test/performance_test/table_data_gen"

def conn():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(120)
    s.connect(('127.0.0.1', 8765))
    return s

def sql(s, q, t=120):
    if q.strip() and not q.strip().endswith(';'): q += ';'
    s.settimeout(t)
    s.sendall((q + '\0').encode())
    b = []
    while True:
        try: d = s.recv(65536)
        except: return 'TIMEOUT'
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

def tsql(s, q):
    t = time.time()
    r = sql(s, q)
    return (time.time() - t) * 1000, r

os.chdir(DBDIR)
dp = os.path.join(DBDIR, DB)
if os.path.exists(dp): shutil.rmtree(dp)
rmdb = subprocess.Popen([RMDB, DB], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(3)

s = conn()
sql(s, 'set output_file off')
for q in [
    'create table warehouse (w_id int, w_name char(10), w_street_1 char(20), w_street_2 char(20), w_city char(20), w_state char(2), w_zip char(9), w_tax float, w_ytd float)',
    'create table district (d_id int, d_w_id int, d_name char(10), d_street_1 char(20), d_street_2 char(20), d_city char(20), d_state char(2), d_zip char(9), d_tax float, d_ytd float, d_next_o_id int)',
    'create table customer (c_id int, c_d_id int, c_w_id int, c_first char(16), c_middle char(2), c_last char(16), c_street_1 char(20), c_street_2 char(20), c_city char(20), c_state char(2), c_zip char(9), c_phone char(16), c_since char(30), c_credit char(2), c_credit_lim int, c_discount float, c_balance float, c_ytd_payment float, c_payment_cnt int, c_delivery_cnt int, c_data char(50))',
    'create table new_orders (no_o_id int, no_d_id int, no_w_id int)',
    'create table orders (o_id int, o_d_id int, o_w_id int, o_c_id int, o_entry_d char(19), o_carrier_id int, o_ol_cnt int, o_all_local int)',
    'create table order_line (ol_o_id int, ol_d_id int, ol_w_id int, ol_number int, ol_i_id int, ol_supply_w_id int, ol_delivery_d char(30), ol_quantity int, ol_amount float, ol_dist_info char(24))',
    'create table item (i_id int, i_im_id int, i_name char(24), i_price float, i_data char(50))',
    'create table stock (s_i_id int, s_w_id int, s_quantity int, s_dist_01 char(24), s_dist_02 char(24), s_dist_03 char(24), s_dist_04 char(24), s_dist_05 char(24), s_dist_06 char(24), s_dist_07 char(24), s_dist_08 char(24), s_dist_09 char(24), s_dist_10 char(24), s_ytd float, s_order_cnt int, s_remote_cnt int, s_data char(50))',
]: sql(s, q)
for c in ['warehouse','district','customer','item','stock']:
    sql(s, f'load {DATA}/{c}.csv into {c}', 600)
for q in ['create index warehouse(w_id)','create index district(d_w_id, d_id)','create index customer(c_w_id, c_d_id, c_id)','create index item(i_id)','create index stock(s_w_id, s_i_id)','create index orders(o_w_id, o_d_id, o_id)','create index new_orders(no_w_id, no_d_id, no_o_id)','create index order_line(ol_w_id, ol_d_id, ol_o_id, ol_number)']:
    sql(s, q, 300)
sql(s, 'set output_file off')
s.close()
print('[1] Data loaded!')

print('\n[2] Single-thread NewOrder SQL Breakdown:')
s = conn()
sql(s, 'set output_file off')
sql(s, 'set transaction isolation level SNAPSHOT ISOLATION')
rng = random.Random(42)
for trial in range(3):
    w = 1; d = rng.randint(1, 10); c = rng.randint(1, 3000)
    timings = {}
    ms, _ = tsql(s, 'begin'); timings['begin'] = ms
    ms, _ = tsql(s, f'select c_discount from customer, warehouse where w_id={w} and c_w_id=w_id and c_d_id={d} and c_id={c}'); timings['cust_wh'] = ms
    ms, r = tsql(s, f'select d_next_o_id from district where d_id={d} and d_w_id={w}'); timings['sel_dist'] = ms; oid = fi(r)
    if oid is None:
        print(f'  Trial {trial+1}: oid is None, skipping')
        s.sendall(b'abort\0'); s.recv(65536)
        continue
    ms, _ = tsql(s, f'update district set d_next_o_id={oid+1} where d_id={d} and d_w_id={w}'); timings['upd_dist'] = ms
    ms, _ = tsql(s, f'insert into orders values ({oid},{d},{w},{c},\'2026-06-30 00:00:00\',0,5,1)'); timings['ins_ord'] = ms
    ms, _ = tsql(s, f'insert into new_orders values ({oid},{d},{w})'); timings['ins_no'] = ms
    a=b=c2=d2=0
    for n in range(1,6):
        iid=rng.randint(1,100000)
        ms,_=tsql(s, f'select i_price from item where i_id={iid}'); a+=ms
        ms,_=tsql(s, f'select s_quantity from stock where s_i_id={iid} and s_w_id={w}'); b+=ms
        ms,_=tsql(s, f'update stock set s_quantity=45 where s_i_id={iid} and s_w_id={w}'); c2+=ms
        ms,_=tsql(s, f'insert into order_line values ({oid},{d},{w},{n},{iid},{w},\'null\',5,5.0,\'dist\')'); d2+=ms
    timings['ol_item_x5']=a; timings['ol_stock_sel_x5']=b; timings['ol_stock_upd_x5']=c2; timings['ol_insert_x5']=d2
    ms,_=tsql(s, 'commit'); timings['commit']=ms
    total=sum(timings.values())
    print(f'  Trial {trial+1}: total={total:.1f}ms')
    for k,v in timings.items():
        print(f'    {k:20s} {v:8.1f}ms ({v/total*100:5.1f}%)')
s.close()

print('\n[3] Multi-thread test (5s per config):')
print(f'{"Threads":>7} | {"Commits":>7} | {"Aborts":>6} | {"Abort%":>6} | {"tpmC":>7}')
print('-' * 50)
for nt in [1, 2, 4, 8, 16]:
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
                w=1; d=rng.randint(1,10); c=rng.randint(1,3000)
                sql(ws, 'begin')
                try:
                    sql(ws, f'select c_discount from customer where c_w_id={w} and c_d_id={d} and c_id={c}')
                    r=sql(ws, f'select d_next_o_id from district where d_id={d} and d_w_id={w}')
                    oid=fi(r)
                    if oid is None: sql(ws,'abort'); lock.__enter__(); stats['abort']+=1; lock.__exit__(None,None,None); continue
                    r=sql(ws, f'update district set d_next_o_id={oid+1} where d_id={d} and d_w_id={w}')
                    if 'abort' in r.lower(): sql(ws,'abort'); lock.__enter__(); stats['abort']+=1; lock.__exit__(None,None,None); continue
                    r=sql(ws, f'insert into orders values ({oid},{d},{w},{c},\'2026-06-30 00:00:00\',0,5,1)')
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
                    with lock:
                        if 'abort' in r.lower(): stats['abort']+=1
                        else: stats['commit']+=1
                except:
                    with lock: stats['error']+=1
                    try: sql(ws,'abort')
                    except: pass
            ws.close()
        except: pass
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(nt)]
    for t in threads: t.start()
    time.sleep(3); stats.clear()
    start=time.time(); time.sleep(5); elapsed=time.time()-start
    stop.set()
    for t in threads: t.join(timeout=5)
    total=stats['commit']+stats['abort']
    ar=stats['abort']/total*100 if total else 0
    tpmC=stats['commit']/(elapsed/60)
    c=stats['commit']; a=stats['abort']
    print(f'{nt:7d} | {c:7d} | {a:6d} | {ar:5.1f}% | {tpmC:7.1f}')

rmdb.terminate()
rmdb.wait(timeout=5)
shutil.rmtree(dp, ignore_errors=True)
print('\nDone!')
