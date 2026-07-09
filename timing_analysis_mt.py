#!/usr/bin/env python3
"""多线程NewOrder逐SQL计时分析 - 需要先启动rmdb并加载数据"""
import socket, time, threading, random, collections

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

def run_trial(num_threads, duration=10):
    """运行一次多线程测试，返回每条SQL的平均延迟"""
    stop = threading.Event()
    timings_lock = threading.Lock()
    # 每条SQL的累计时间和次数
    sql_timings = collections.defaultdict(lambda: [0.0, 0])
    stats = {'commit': 0, 'abort': 0}

    def worker(tid):
        try:
            ws = conn()
            sql(ws, 'set output_file off')
            sql(ws, 'set transaction isolation level SNAPSHOT ISOLATION')
            rng = random.Random(tid)
            while not stop.is_set():
                w = 1; d = rng.randint(1, 10); c = rng.randint(1, 3000)
                local_timings = {}
                
                ms, _ = tsql(ws, 'begin')
                local_timings['begin'] = ms
                
                ms, _ = tsql(ws, f'select c_discount, c_last, c_credit, w_tax from customer, warehouse where w_id={w} and c_w_id=w_id and c_d_id={d} and c_id={c}')
                local_timings['cust_wh'] = ms
                
                ms, r = tsql(ws, f'select d_next_o_id, d_tax from district where d_id={d} and d_w_id={w}')
                local_timings['sel_dist'] = ms
                oid = fi(r)
                if oid is None:
                    ws.sendall(b'abort\0'); ws.recv(65536)
                    with timings_lock: stats['abort'] += 1
                    continue
                
                ms, r = tsql(ws, f'update district set d_next_o_id={oid+1} where d_id={d} and d_w_id={w}')
                local_timings['upd_dist'] = ms
                if 'abort' in r.lower():
                    ws.sendall(b'abort\0'); ws.recv(65536)
                    with timings_lock: stats['abort'] += 1
                    continue
                
                ms, r = tsql(ws, f'insert into orders values ({oid},{d},{w},{c},\'2026-06-30 00:00:00\',0,5,1)')
                local_timings['ins_ord'] = ms
                if 'abort' in r.lower():
                    ws.sendall(b'abort\0'); ws.recv(65536)
                    with timings_lock: stats['abort'] += 1
                    continue
                
                ms, r = tsql(ws, f'insert into new_orders values ({oid},{d},{w})')
                local_timings['ins_no'] = ms
                if 'abort' in r.lower():
                    ws.sendall(b'abort\0'); ws.recv(65536)
                    with timings_lock: stats['abort'] += 1
                    continue
                
                ol_sel_item = ol_sel_stock = ol_upd_stock = ol_ins_ol = 0
                fail = False
                for n in range(1, 6):
                    iid = rng.randint(1, 100000)
                    ms, _ = tsql(ws, f'select i_price, i_name, i_data from item where i_id={iid}')
                    ol_sel_item += ms
                    ms, _ = tsql(ws, f'select s_quantity, s_data from stock where s_i_id={iid} and s_w_id={w}')
                    ol_sel_stock += ms
                    ms, r = tsql(ws, f'update stock set s_quantity=45 where s_i_id={iid} and s_w_id={w}')
                    ol_upd_stock += ms
                    if 'abort' in r.lower():
                        fail = True; break
                    ms, r = tsql(ws, f'insert into order_line values ({oid},{d},{w},{n},{iid},{w},\'null\',5,5.0,\'dist\')')
                    ol_ins_ol += ms
                    if 'abort' in r.lower():
                        fail = True; break
                
                local_timings['ol_item_x5'] = ol_sel_item
                local_timings['ol_stock_sel_x5'] = ol_sel_stock
                local_timings['ol_stock_upd_x5'] = ol_upd_stock
                local_timings['ol_insert_x5'] = ol_ins_ol
                
                if fail:
                    ws.sendall(b'abort\0'); ws.recv(65536)
                    with timings_lock: stats['abort'] += 1
                    continue
                
                ms, r = tsql(ws, 'commit')
                local_timings['commit'] = ms
                
                with timings_lock:
                    if 'abort' in r.lower():
                        stats['abort'] += 1
                    else:
                        stats['commit'] += 1
                        for k, v in local_timings.items():
                            sql_timings[k][0] += v
                            sql_timings[k][1] += 1
            ws.close()
        except:
            pass

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(num_threads)]
    for t in threads: t.start()
    time.sleep(duration)
    stop.set()
    for t in threads: t.join(timeout=10)
    return stats, sql_timings

# 运行测试
print("Multi-thread NewOrder SQL Timing Analysis")
print("需要先启动rmdb并加载数据(1W,10D,3000C,100000I)\n")

for num_threads in [1, 2, 4, 8, 16]:
    stats, sql_timings = run_trial(num_threads, duration=10)
    total = stats['commit'] + stats['abort']
    ar = stats['abort'] / total * 100 if total > 0 else 0
    tpmC = stats['commit'] / (10 / 60)
    
    print(f"=== {num_threads} threads: {stats['commit']} commits, {stats['abort']} aborts, abort%={ar:.1f}%, tpmC={tpmC:.0f} ===")
    
    # 计算每条SQL平均延迟
    total_avg = 0
    for k in ['begin', 'cust_wh', 'sel_dist', 'upd_dist', 'ins_ord', 'ins_no', 
              'ol_item_x5', 'ol_stock_sel_x5', 'ol_stock_upd_x5', 'ol_insert_x5', 'commit']:
        if k in sql_timings and sql_timings[k][1] > 0:
            avg = sql_timings[k][0] / sql_timings[k][1]
            total_avg += avg
            print(f"  {k:20s} {avg:8.1f}ms")
    print(f"  {'TOTAL':20s} {total_avg:8.1f}ms")
    print()
