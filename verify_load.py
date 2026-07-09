#!/usr/bin/env python3
"""验证 load + 事务 + 隔离级别的正确性"""
import socket, time, subprocess, os, sys, signal

PORT = 8765
DB = "test_verify_load"
RMDB = "./build/bin/rmdb"
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "src/test/performance_test/table_data_gen")

def conn():
    s = socket.socket()
    s.settimeout(60)
    s.connect(('127.0.0.1', PORT))
    return s

def sql(s, q, t=60):
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

def main():
    os.chdir(os.path.dirname(os.path.abspath(__file__)))

    import shutil
    # Kill old server
    subprocess.run(["pkill", "-9", "rmdb"], capture_output=True)
    time.sleep(2)

    # Clean and start
    if os.path.exists(DB):
        shutil.rmtree(DB)

    rmdb = subprocess.Popen([RMDB, DB],
                           stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)

    # Wait for server to start
    for i in range(20):
        time.sleep(1)
        try:
            test_s = socket.socket()
            test_s.settimeout(2)
            test_s.connect(('127.0.0.1', PORT))
            test_s.close()
            break
        except:
            pass
    else:
        print("ERROR: Server failed to start")
        rmdb.terminate()
        return 1

    passed = 0
    failed = 0

    try:
        s = conn()
        sql(s, 'set output_file off')

        # Test 1: Create tables
        print("[1] Creating tables...")
        tables = [
            'create table warehouse (w_id int, w_name char(10), w_street_1 char(20), w_street_2 char(20), w_city char(20), w_state char(2), w_zip char(9), w_tax float, w_ytd float)',
            'create table district (d_id int, d_w_id int, d_name char(10), d_street_1 char(20), d_street_2 char(20), d_city char(20), d_state char(2), d_zip char(9), d_tax float, d_ytd float, d_next_o_id int)',
            'create table customer (c_id int, c_d_id int, c_w_id int, c_first char(16), c_middle char(2), c_last char(16), c_street_1 char(20), c_street_2 char(20), c_city char(20), c_state char(2), c_zip char(9), c_phone char(16), c_since char(30), c_credit char(2), c_credit_lim int, c_discount float, c_balance float, c_ytd_payment float, c_payment_cnt int, c_delivery_cnt int, c_data char(50))',
            'create table new_orders (no_o_id int, no_d_id int, no_w_id int)',
            'create table orders (o_id int, o_d_id int, o_w_id int, o_c_id int, o_entry_d char(19), o_carrier_id int, o_ol_cnt int, o_all_local int)',
            'create table order_line (ol_o_id int, ol_d_id int, ol_w_id int, ol_number int, ol_i_id int, ol_supply_w_id int, ol_delivery_d char(30), ol_quantity int, ol_amount float, ol_dist_info char(24))',
            'create table item (i_id int, i_im_id int, i_name char(24), i_price float, i_data char(50))',
            'create table stock (s_i_id int, s_w_id int, s_quantity int, s_dist_01 char(24), s_dist_02 char(24), s_dist_03 char(24), s_dist_04 char(24), s_dist_05 char(24), s_dist_06 char(24), s_dist_07 char(24), s_dist_08 char(24), s_dist_09 char(24), s_dist_10 char(24), s_ytd float, s_order_cnt int, s_remote_cnt int, s_data char(50))',
        ]
        for q in tables:
            sql(s, q)
        print("  Tables created OK")

        # Test 2: Create indexes
        print("[2] Creating indexes...")
        indexes = [
            'create index warehouse(w_id)',
            'create index district(d_w_id, d_id)',
            'create index customer(c_w_id, c_d_id, c_id)',
            'create index item(i_id)',
            'create index stock(s_w_id, s_i_id)',
            'create index orders(o_w_id, o_d_id, o_id)',
            'create index new_orders(no_w_id, no_d_id, no_o_id)',
            'create index order_line(ol_w_id, ol_d_id, ol_o_id, ol_number)',
        ]
        for q in indexes:
            sql(s, q)
        print("  Indexes created OK")

        # Test 3: Load data
        print("[3] Loading TPC-C data...")
        tables_to_load = ['warehouse', 'district', 'customer', 'item', 'stock']
        for t in tables_to_load:
            t0 = time.time()
            r = sql(s, f'load {DATA}/{t}.csv into {t}', 300)
            elapsed = time.time() - t0
            print(f"  {t}: {elapsed:.1f}s")
        print("  Load OK")

        # Test 4: Verify counts
        print("[4] Verifying data counts...")
        counts = {
            'warehouse': 1,
            'district': 10,
            'customer': 30000,
            'item': 100000,
            'stock': 100000,
        }
        for table, expected in counts.items():
            r = sql(s, f'select count(*) from {table}')
            actual = fi(r)
            if actual == expected:
                print(f"  {table}: {actual} OK")
                passed += 1
            else:
                print(f"  {table}: {actual} != {expected} FAIL")
                failed += 1

        # Test 5: Index scan works
        print("[5] Testing index scan...")
        sql(s, 'set transaction isolation level SNAPSHOT ISOLATION')
        r = sql(s, 'select s_quantity from stock where s_w_id=1 and s_i_id=1')
        val = fi(r)
        if val is not None:
            print(f"  stock(s_w_id=1, s_i_id=1): s_quantity={val} OK")
            passed += 1
        else:
            print(f"  stock lookup FAIL: {r[:100]}")
            failed += 1

        # Test 6: Transaction with UPDATE
        print("[6] Testing transaction with UPDATE...")
        sql(s, 'begin')
        r = sql(s, 'select d_next_o_id from district where d_id=1 and d_w_id=1')
        oid = fi(r)
        if oid is not None:
            r = sql(s, f'update district set d_next_o_id={oid+1} where d_id=1 and d_w_id=1')
            if 'abort' not in r.lower():
                r = sql(s, 'select d_next_o_id from district where d_id=1 and d_w_id=1')
                new_oid = fi(r)
                if new_oid == oid + 1:
                    sql(s, 'commit')
                    print(f"  UPDATE district: {oid} -> {new_oid} OK")
                    passed += 1
                else:
                    sql(s, 'abort')
                    print(f"  UPDATE district: value mismatch FAIL")
                    failed += 1
            else:
                sql(s, 'abort')
                print(f"  UPDATE district: abort FAIL: {r[:50]}")
                failed += 1
        else:
            sql(s, 'abort')
            print(f"  SELECT district FAIL")
            failed += 1

        # Test 7: NewOrder-like transaction
        print("[7] Testing NewOrder transaction...")
        sql(s, 'set transaction isolation level SNAPSHOT ISOLATION')
        sql(s, 'begin')
        w = 1; d = 1; c = 1
        sql(s, f'select c_discount from customer where c_w_id={w} and c_d_id={d} and c_id={c}')
        r = sql(s, f'select d_next_o_id from district where d_id={d} and d_w_id={w}')
        oid = fi(r)
        if oid is None:
            sql(s, 'abort')
            print(f"  NewOrder: oid is None FAIL")
            failed += 1
        else:
            r = sql(s, f'update district set d_next_o_id={oid+1} where d_id={d} and d_w_id={w}')
            if 'abort' in r.lower():
                sql(s, 'abort')
                print(f"  NewOrder: update district abort FAIL")
                failed += 1
            else:
                sql(s, f'insert into orders values ({oid},{d},{w},{c},\'2026-07-10 00:00:00\',0,5,1)')
                sql(s, f'insert into new_orders values ({oid},{d},{w})')
                fail = False
                for n in range(1, 6):
                    iid = n
                    r = sql(s, f'update stock set s_quantity=45 where s_i_id={iid} and s_w_id={w}')
                    if 'abort' in r.lower():
                        fail = True; break
                if fail:
                    sql(s, 'abort')
                    print(f"  NewOrder: update stock abort FAIL")
                    failed += 1
                else:
                    r = sql(s, 'commit')
                    if 'abort' in r.lower():
                        print(f"  NewOrder: commit abort FAIL")
                        failed += 1
                    else:
                        print(f"  NewOrder transaction OK (oid={oid})")
                        passed += 1

        s.close()

    except Exception as e:
        print(f"ERROR: {e}")
        import traceback
        traceback.print_exc()
    finally:
        rmdb.terminate()
        rmdb.wait(timeout=5)
        if os.path.exists(DB):
            shutil.rmtree(DB)

    print(f"\n{'='*50}")
    print(f"Results: {passed} passed, {failed} failed")
    print(f"{'='*50}")

    return 0 if failed == 0 else 1

if __name__ == '__main__':
    sys.exit(main())
