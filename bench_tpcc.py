#!/usr/bin/env python3
"""本地 TPC-C 风格性能基准测试"""
import socket
import subprocess
import time
import threading
import sys

def send_sql(sock, sql):
    sock.sendall((sql + '\0').encode())
    return sock.recv(65536).decode(errors='replace')

def create_connection(port=8765):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(30)
    s.connect(("127.0.0.1", port))
    send_sql(s, 'SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;')
    time.sleep(0.05)
    return s

def setup_db(db_name):
    """创建 TPC-C 风格的表结构"""
    subprocess.run(['pkill', '-f', f'rmdb {db_name}'], capture_output=True)
    time.sleep(0.3)
    subprocess.run(['rm', '-rf', db_name], capture_output=True)

    server = subprocess.Popen(
        ['./build/bin/rmdb', db_name],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(1)

    sock = create_connection()

    # 创建表
    tables = [
        "CREATE TABLE warehouse (w_id int, w_name char(10), w_street_1 char(20), w_street_2 char(20), w_city char(20), w_state char(2), w_zip char(9), w_tax float, w_ytd float);",
        "CREATE TABLE district (d_w_id int, d_id int, d_name char(10), d_street_1 char(20), d_street_2 char(20), d_city char(20), d_state char(2), d_zip char(9), d_tax float, d_ytd float, d_next_o_id int);",
        "CREATE TABLE customer (c_w_id int, c_d_id int, c_id int, c_first char(16), c_middle char(2), c_last char(16), c_street_1 char(20), c_street_2 char(20), c_city char(20), c_state char(2), c_zip char(9), c_phone char(16), c_since char(30), c_credit char(2), c_credit_lim float, c_discount float, c_balance float, c_ytd_payment float, c_payment_cnt int, c_delivery_cnt int, c_data char(50));",
        "CREATE TABLE orders (o_w_id int, o_d_id int, o_id int, o_c_id int, o_entry_d char(30), o_carrier_id int, o_ol_cnt int, o_all_local int);",
        "CREATE TABLE new_orders (no_w_id int, no_d_id int, no_o_id int);",
        "CREATE TABLE order_line (ol_w_id int, ol_d_id int, ol_o_id int, ol_number int, ol_i_id int, ol_supply_w_id int, ol_delivery_d char(30), ol_quantity int, ol_amount float, ol_dist_info char(24));",
        "CREATE TABLE stock (s_w_id int, s_i_id int, s_quantity int, s_dist_01 char(24), s_dist_02 char(24), s_dist_03 char(24), s_dist_04 char(24), s_dist_05 char(24), s_dist_06 char(24), s_dist_07 char(24), s_dist_08 char(24), s_dist_09 char(24), s_dist_10 char(24), s_ytd int, s_order_cnt int, s_remote_cnt int, s_data char(50));",
        "CREATE TABLE item (i_id int, i_name char(24), i_price float, i_data char(50));",
        "CREATE TABLE history (h_c_w_id int, h_c_d_id int, h_c_id int, h_w_id int, h_d_id int, h_date char(30), h_amount float, h_data char(24));",
    ]

    for sql in tables:
        send_sql(sock, sql)
        time.sleep(0.05)

    # 创建索引
    indexes = [
        "CREATE INDEX warehouse (w_id);",
        "CREATE INDEX district (d_w_id, d_id);",
        "CREATE INDEX customer (c_w_id, c_d_id, c_id);",
        "CREATE INDEX orders (o_w_id, o_d_id, o_id);",
        "CREATE INDEX new_orders (no_w_id, no_d_id, no_o_id);",
        "CREATE INDEX order_line (ol_w_id, ol_d_id, ol_o_id, ol_number);",
        "CREATE INDEX stock (s_w_id, s_i_id);",
        "CREATE INDEX item (i_id);",
    ]

    for sql in indexes:
        send_sql(sock, sql)
        time.sleep(0.05)

    sock.close()
    return server

def load_data(num_warehouses=1):
    """加载初始数据"""
    sock = create_connection()

    # Load warehouse
    for w in range(1, num_warehouses + 1):
        send_sql(sock, f"INSERT INTO warehouse VALUES ({w}, 'wname', 'street1', 'street2', 'city', 'ST', '123456789', 0.10, 300000.00);")
        time.sleep(0.01)

    # Load districts (10 per warehouse)
    for w in range(1, num_warehouses + 1):
        for d in range(1, 11):
            send_sql(sock, f"INSERT INTO district VALUES ({w}, {d}, 'dname', 'dst1', 'dst2', 'city', 'ST', '123456789', 0.10, 30000.00, 3001);")
            time.sleep(0.01)

    # Load some customers (simplified - 100 per district)
    for w in range(1, num_warehouses + 1):
        for d in range(1, 11):
            for c in range(1, 101):
                send_sql(sock, f"INSERT INTO customer VALUES ({w}, {d}, {c}, 'first', 'OE', 'last', 'st1', 'st2', 'city', 'ST', '123456789', 'phone', '2020-01-01', 'GC', 50000.00, 0.10, -10.00, 10.00, 1, 0, 'data');")
                time.sleep(0.005)

    # Load items (1000)
    for i in range(1, 1001):
        send_sql(sock, f"INSERT INTO item VALUES ({i}, 'item{i}', {i * 0.5:.2f}, 'data');")
        time.sleep(0.005)

    # Load stock (1000 per warehouse)
    for w in range(1, num_warehouses + 1):
        for i in range(1, 1001):
            send_sql(sock, f"INSERT INTO stock VALUES ({w}, {i}, 100, 'dist01', 'dist02', 'dist03', 'dist04', 'dist05', 'dist06', 'dist07', 'dist08', 'dist09', 'dist10', 0, 0, 0, 'data');")
            time.sleep(0.003)

    sock.close()
    print(f"Loaded data for {num_warehouses} warehouse(s)")

def new_order_txn(sock, w_id, d_id):
    """新订单事务"""
    send_sql(sock, 'BEGIN;')
    time.sleep(0.01)

    # Read district and get next order id
    resp = send_sql(sock, f'SELECT d_next_o_id FROM district WHERE d_w_id = {w_id} AND d_id = {d_id};')
    time.sleep(0.01)
    # Parse the order id from response
    lines = resp.strip().split('\n')
    o_id = 3001  # default
    for line in lines:
        line = line.strip().strip('|').strip()
        if line.isdigit():
            o_id = int(line)
            break

    # Update district
    resp = send_sql(sock, f'UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_w_id = {w_id} AND d_id = {d_id};')
    time.sleep(0.01)
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # Insert order
    resp = send_sql(sock, f"INSERT INTO orders VALUES ({w_id}, {d_id}, {o_id}, 1, '2020-01-01', 0, 5, 1);")
    time.sleep(0.01)
    if 'abort' in resp.lower() or 'error' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # Insert new_order
    resp = send_sql(sock, f'INSERT INTO new_orders VALUES ({w_id}, {d_id}, {o_id});')
    time.sleep(0.01)
    if 'abort' in resp.lower() or 'error' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # Insert order lines
    for ol in range(1, 6):
        resp = send_sql(sock, f"INSERT INTO order_line VALUES ({w_id}, {d_id}, {o_id}, {ol}, {ol}, {w_id}, '', 5, {ol * 5.0:.2f}, 'dist');")
        time.sleep(0.01)
        if 'abort' in resp.lower() or 'error' in resp.lower():
            send_sql(sock, 'ABORT;')
            return False

    # Update stock
    for ol in range(1, 6):
        resp = send_sql(sock, f'UPDATE stock SET s_quantity = s_quantity - 1, s_ytd = s_ytd + 1, s_order_cnt = s_order_cnt + 1 WHERE s_w_id = {w_id} AND s_i_id = {ol};')
        time.sleep(0.01)
        if 'abort' in resp.lower() or 'error' in resp.lower():
            send_sql(sock, 'ABORT;')
            return False

    send_sql(sock, 'COMMIT;')
    return True

def payment_txn(sock, w_id, d_id):
    """支付事务"""
    send_sql(sock, 'BEGIN;')
    time.sleep(0.01)

    resp = send_sql(sock, f'UPDATE warehouse SET w_ytd = w_ytd + 1.00 WHERE w_id = {w_id};')
    time.sleep(0.01)
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    resp = send_sql(sock, f'UPDATE district SET d_ytd = d_ytd + 1.00 WHERE d_w_id = {w_id} AND d_id = {d_id};')
    time.sleep(0.01)
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    resp = send_sql(sock, f'UPDATE customer SET c_balance = c_balance - 1.00, c_ytd_payment = c_ytd_payment + 1.00, c_payment_cnt = c_payment_cnt + 1 WHERE c_w_id = {w_id} AND c_d_id = {d_id} AND c_id = 1;')
    time.sleep(0.01)
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    send_sql(sock, 'COMMIT;')
    return True

def worker(wid, duration, results, port=8765):
    """Worker 线程执行 TPC-C 事务"""
    try:
        sock = create_connection(port)
        new_orders = 0
        payments = 0
        aborts = 0
        start = time.time()

        while time.time() - start < duration:
            w_id = 1
            d_id = (wid % 10) + 1

            # Alternate between new_order and payment
            if (new_orders + payments) % 2 == 0:
                if new_order_txn(sock, w_id, d_id):
                    new_orders += 1
                else:
                    aborts += 1
            else:
                if payment_txn(sock, w_id, d_id):
                    payments += 1
                else:
                    aborts += 1

        sock.close()
        results.append((wid, new_orders, payments, aborts))
    except Exception as e:
        results.append((wid, 0, 0, str(e)))

def run_benchmark(num_workers=8, duration=30, port=8765):
    """运行基准测试"""
    results = []
    threads = []

    start = time.time()
    for i in range(num_workers):
        t = threading.Thread(target=worker, args=(i, duration, results, port))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()
    elapsed = time.time() - start

    total_new_orders = sum(r[1] for r in results if isinstance(r[1], int))
    total_payments = sum(r[2] for r in results if isinstance(r[2], int))
    total_aborts = sum(r[3] for r in results if isinstance(r[3], int))
    total_txns = total_new_orders + total_payments

    tpmc = total_new_orders / (elapsed / 60)
    throughput = total_txns / elapsed
    abort_rate = total_aborts / (total_txns + total_aborts) * 100 if (total_txns + total_aborts) > 0 else 0

    print(f"\n{'='*50}")
    print(f"Benchmark Results ({num_workers} workers, {duration}s)")
    print(f"{'='*50}")
    print(f"NewOrder txns:    {total_new_orders}")
    print(f"Payment txns:     {total_payments}")
    print(f"Total txns:       {total_txns}")
    print(f"Aborted txns:     {total_aborts}")
    print(f"Abort rate:       {abort_rate:.1f}%")
    print(f"Elapsed:          {elapsed:.1f}s")
    print(f"tpmC:             {tpmc:.1f}")
    print(f"Throughput:       {throughput:.1f} txns/sec")
    print(f"{'='*50}")

    return tpmc, throughput, abort_rate

if __name__ == "__main__":
    db_name = "bench_tpcc_db"
    num_workers = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else 30

    print("Setting up database...")
    server = setup_db(db_name)

    print("Loading data...")
    load_data(num_warehouses=1)

    print(f"Running benchmark ({num_workers} workers, {duration}s)...")
    tpmc, throughput, abort_rate = run_benchmark(num_workers, duration)

    # Cleanup
    subprocess.run(['pkill', '-f', f'rmdb {db_name}'], capture_output=True)
    server.wait(timeout=3)
    subprocess.run(['rm', '-rf', db_name], capture_output=True)
