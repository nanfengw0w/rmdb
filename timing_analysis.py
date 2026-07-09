#!/usr/bin/env python3
"""NewOrder逐SQL计时 - 需要先启动rmdb并加载数据"""
import socket, time, random

PORT = 8765

def conn():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(120)
    s.connect(('127.0.0.1', PORT))
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

s = conn()
sql(s, 'set output_file off')
sql(s, 'set transaction isolation level SNAPSHOT ISOLATION')

rng = random.Random(42)
print("NewOrder SQL Breakdown (5 trials):")
for trial in range(5):
    w = 1; d = rng.randint(1, 10); c = rng.randint(1, 3000)
    timings = {}
    
    ms, _ = tsql(s, 'begin')
    timings['begin'] = ms
    
    ms, _ = tsql(s, f'select c_discount, c_last, c_credit, w_tax from customer, warehouse where w_id={w} and c_w_id=w_id and c_d_id={d} and c_id={c}')
    timings['cust_wh_join'] = ms
    
    ms, r = tsql(s, f'select d_next_o_id, d_tax from district where d_id={d} and d_w_id={w}')
    timings['sel_district'] = ms
    oid = fi(r)
    
    ms, _ = tsql(s, f'update district set d_next_o_id={oid+1} where d_id={d} and d_w_id={w}')
    timings['upd_district'] = ms
    
    ms, _ = tsql(s, f'insert into orders values ({oid},{d},{w},{c},\'2026-06-30 00:00:00\',0,5,1)')
    timings['ins_orders'] = ms
    
    ms, _ = tsql(s, f'insert into new_orders values ({oid},{d},{w})')
    timings['ins_new_orders'] = ms
    
    ol_item = ol_stock_sel = ol_stock_upd = ol_ol = 0
    for n in range(1, 6):
        iid = rng.randint(1, 100000)
        ms, _ = tsql(s, f'select i_price, i_name, i_data from item where i_id={iid}')
        ol_item += ms
        ms, _ = tsql(s, f'select s_quantity, s_data, s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05, s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10 from stock where s_i_id={iid} and s_w_id={w}')
        ol_stock_sel += ms
        ms, _ = tsql(s, f'update stock set s_quantity=45 where s_i_id={iid} and s_w_id={w}')
        ol_stock_upd += ms
        ms, _ = tsql(s, f'insert into order_line values ({oid},{d},{w},{n},{iid},{w},\'null\',5,5.0,\'dist\')')
        ol_ol += ms
    
    timings['ol_item_x5'] = ol_item
    timings['ol_stock_sel_x5'] = ol_stock_sel
    timings['ol_stock_upd_x5'] = ol_stock_upd
    timings['ol_insert_x5'] = ol_ol
    
    ms, _ = tsql(s, 'commit')
    timings['commit'] = ms
    
    total = sum(timings.values())
    print(f"\n  Trial {trial+1}: total={total:.1f}ms")
    for k, v in timings.items():
        pct = v / total * 100
        bar = '#' * int(pct / 2)
        print(f"    {k:20s} {v:8.1f}ms ({pct:5.1f}%) {bar}")

s.close()
