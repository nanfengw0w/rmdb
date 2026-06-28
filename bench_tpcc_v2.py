#!/usr/bin/env python3
"""
TPC-C 风格性能基准测试 v2
匹配真实评测的表结构和事务逻辑
"""
import socket
import subprocess
import time
import threading
import sys
import random
import csv
import os

PORT = 8765

def send_sql(sock, sql, timeout=30):
    sock.settimeout(timeout)
    sock.sendall((sql + '\0').encode())
    resp = sock.recv(65536).decode(errors='replace')
    return resp

def create_connection(port=PORT):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(30)
    s.connect(("127.0.0.1", port))
    send_sql(s, 'SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;')
    time.sleep(0.02)
    return s

# ============================================================
# 表结构 (匹配真实 CSV 文件的列)
# ============================================================
TABLES = {
    "warehouse": "CREATE TABLE warehouse (w_id int, w_name char(10), w_street_1 char(20), w_street_2 char(20), w_city char(20), w_state char(2), w_zip char(9), w_tax float, w_ytd float);",
    "district": "CREATE TABLE district (d_id int, d_w_id int, d_name char(10), d_street_1 char(20), d_street_2 char(20), d_city char(20), d_state char(2), d_zip char(9), d_tax float, d_ytd float, d_next_o_id int);",
    "customer": "CREATE TABLE customer (c_id int, c_d_id int, c_w_id int, c_first char(16), c_middle char(2), c_last char(16), c_street_1 char(20), c_street_2 char(20), c_city char(20), c_state char(2), c_zip char(9), c_phone char(16), c_since char(30), c_credit char(2), c_credit_lim float, c_discount float, c_balance float, c_ytd_payment float, c_payment_cnt int, c_delivery_cnt int, c_data char(50));",
    "history": "CREATE TABLE history (h_c_id int, h_c_d_id int, h_c_w_id int, h_d_id int, h_w_id int, h_date char(30), h_amount float, h_data char(24));",
    "orders": "CREATE TABLE orders (o_id int, o_d_id int, o_w_id int, o_c_id int, o_entry_d char(30), o_carrier_id int, o_ol_cnt int, o_all_local int);",
    "new_orders": "CREATE TABLE new_orders (no_o_id int, no_d_id int, no_w_id int);",
    "order_line": "CREATE TABLE order_line (ol_o_id int, ol_d_id int, ol_w_id int, ol_number int, ol_i_id int, ol_supply_w_id int, ol_delivery_d char(30), ol_quantity int, ol_amount float, ol_dist_info char(24));",
    "item": "CREATE TABLE item (i_id int, i_im_id int, i_name char(24), i_price float, i_data char(50));",
    "stock": "CREATE TABLE stock (s_i_id int, s_w_id int, s_quantity int, s_dist_01 char(24), s_dist_02 char(24), s_dist_03 char(24), s_dist_04 char(24), s_dist_05 char(24), s_dist_06 char(24), s_dist_07 char(24), s_dist_08 char(24), s_dist_09 char(24), s_dist_10 char(24), s_ytd int, s_order_cnt int, s_remote_cnt int, s_data char(50));",
}

INDEXES = [
    "CREATE INDEX warehouse (w_id);",
    "CREATE INDEX district (d_id, d_w_id);",
    "CREATE INDEX customer (c_id, c_d_id, c_w_id);",
    # history 不建索引 (允许同一客户多次付款)
    "CREATE INDEX orders (o_id, o_d_id, o_w_id);",
    "CREATE INDEX new_orders (no_o_id, no_d_id, no_w_id);",
    "CREATE INDEX order_line (ol_o_id, ol_d_id, ol_w_id, ol_number);",
    "CREATE INDEX item (i_id);",
    "CREATE INDEX stock (s_i_id, s_w_id);",
]

# ============================================================
# 数据加载 (使用 load 命令或手动插入)
# ============================================================
def setup_and_load(db_name, use_csv=True, csv_dir=None):
    """创建数据库、建表、加载数据"""
    subprocess.run(['pkill', '-f', f'rmdb {db_name}'], capture_output=True)
    time.sleep(0.3)
    subprocess.run(['rm', '-rf', db_name], capture_output=True)

    server = subprocess.Popen(
        [f'./build/bin/rmdb', db_name],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(2)  # 等待服务器完全启动

    sock = create_connection()

    # 建表
    for name, sql in TABLES.items():
        resp = send_sql(sock, sql)
        if 'failure' in resp.lower() or 'error' in resp.lower():
            print(f"Create table {name} failed: {resp.strip()[:80]}")
        time.sleep(0.05)

    # 建索引
    for sql in INDEXES:
        resp = send_sql(sock, sql)
        if 'failure' in resp.lower() or 'error' in resp.lower():
            print(f"Create index failed: {resp.strip()[:80]}")
        time.sleep(0.05)

    # 加载数据
    if use_csv and csv_dir:
        load_csv_data(sock, csv_dir)
    else:
        load_synthetic_data(sock)

    sock.close()
    return server

def load_csv_data(sock, csv_dir):
    """使用 load 命令加载 CSV 数据"""
    tables = ['warehouse', 'district', 'customer', 'history', 'orders',
              'new_orders', 'order_line', 'item', 'stock']
    for table in tables:
        csv_path = os.path.join(csv_dir, f'{table}.csv')
        if os.path.exists(csv_path):
            # load 命令使用相对于数据库文件夹的路径
            # 需要将绝对路径转为相对路径
            resp = send_sql(sock, f'load {csv_path} into {table};', timeout=120)
            if 'failure' in resp.lower() or 'error' in resp.lower():
                print(f"Load {table} failed: {resp.strip()[:80]}")
            else:
                print(f"Loaded {table}")
            time.sleep(0.1)

def load_synthetic_data(sock):
    """生成合成数据 (小规模，用于快速测试)"""
    W = 1  # warehouse count
    ITEMS = 100  # item count (真实是 100000)
    CUSTOMERS_PER_DIST = 10  # customer per district (真实是 3000)
    DISTRICTS_PER_WH = 10

    print("Loading synthetic data...")

    # warehouse
    for w in range(1, W + 1):
        send_sql(sock, f"INSERT INTO warehouse VALUES ({w}, 'wname', 'st1', 'st2', 'city', 'ST', '123456789', 0.10, 300000.00);")
    print(f"  warehouse: {W}")

    # item
    for i in range(1, ITEMS + 1):
        send_sql(sock, f"INSERT INTO item VALUES ({i}, {i}, 'item{i}', {i * 0.5:.2f}, 'data');")
    print(f"  item: {ITEMS}")

    # stock
    cnt = 0
    for w in range(1, W + 1):
        for i in range(1, ITEMS + 1):
            send_sql(sock, f"INSERT INTO stock VALUES ({i}, {w}, 100, 'd01', 'd02', 'd03', 'd04', 'd05', 'd06', 'd07', 'd08', 'd09', 'd10', 0, 0, 0, 'data');")
            cnt += 1
    print(f"  stock: {cnt}")

    # district, customer, orders, new_orders, order_line, history
    order_id = 1
    for w in range(1, W + 1):
        for d in range(1, DISTRICTS_PER_WH + 1):
            send_sql(sock, f"INSERT INTO district VALUES ({d}, {w}, 'dname', 'dst1', 'dst2', 'city', 'ST', '123456789', 0.10, 30000.00, {CUSTOMERS_PER_DIST + 1});")
            for c in range(1, CUSTOMERS_PER_DIST + 1):
                send_sql(sock, f"INSERT INTO customer VALUES ({c}, {d}, {w}, 'first', 'OE', 'last', 'st1', 'st2', 'city', 'ST', '123456789', 'phone', '2020-01-01', 'GC', 50000.00, 0.10, -10.00, 10.00, 1, 0, 'data');")
                send_sql(sock, f"INSERT INTO history VALUES ({c}, {d}, {w}, {d}, {w}, '2020-01-01', 10.00, 'data');")
                # orders
                o_id = order_id
                ol_cnt = random.randint(5, 15)
                send_sql(sock, f"INSERT INTO orders VALUES ({o_id}, {d}, {w}, {c}, '2020-01-01', {random.randint(1, 10)}, {ol_cnt}, 1);")
                # new_orders (last 900 per district, but we have fewer)
                if o_id > CUSTOMERS_PER_DIST - 3:
                    send_sql(sock, f"INSERT INTO new_orders VALUES ({o_id}, {d}, {w});")
                # order_line
                for ol in range(1, ol_cnt + 1):
                    send_sql(sock, f"INSERT INTO order_line VALUES ({o_id}, {d}, {w}, {ol}, {random.randint(1, ITEMS)}, {w}, '2020-01-01', 5, {ol * 1.5:.2f}, 'dist');")
                order_id += 1
    print(f"  district: {W * DISTRICTS_PER_WH}")
    print(f"  customer: {W * DISTRICTS_PER_WH * CUSTOMERS_PER_DIST}")
    print(f"  orders: {order_id - 1}")

# ============================================================
# TPC-C 事务实现
# ============================================================

def new_order_txn(sock, w_id, d_id, c_id, items):
    """
    NewOrder 事务
    items: list of (ol_i_id, ol_quantity, ol_supply_w_id)
    """
    send_sql(sock, 'BEGIN;')

    # 1. 读取 district
    resp = send_sql(sock, f'SELECT d_next_o_id, d_tax FROM district WHERE d_id = {d_id} AND d_w_id = {w_id};')
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # 解析 o_id
    lines = resp.strip().split('\n')
    o_id = 3001
    d_tax = 0.0
    for line in lines:
        parts = [p.strip() for p in line.strip('|').split('|') if p.strip()]
        if len(parts) >= 2 and parts[0].replace('.', '').replace('-', '').isdigit():
            o_id = int(float(parts[0]))
            try:
                d_tax = float(parts[1])
            except:
                pass
            break

    # 2. 更新 district d_next_o_id
    resp = send_sql(sock, f'UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_id = {d_id} AND d_w_id = {w_id};')
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # 3. 读取 customer
    resp = send_sql(sock, f'SELECT c_discount, c_last, c_credit FROM customer WHERE c_id = {c_id} AND c_d_id = {d_id} AND c_w_id = {w_id};')
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # 4. 读取 warehouse
    resp = send_sql(sock, f'SELECT w_tax FROM warehouse WHERE w_id = {w_id};')
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # 5. 插入 orders
    ol_cnt = len(items)
    resp = send_sql(sock, f"INSERT INTO orders VALUES ({o_id}, {d_id}, {w_id}, {c_id}, '2020-01-01', 0, {ol_cnt}, 1);")
    if 'abort' in resp.lower() or 'error' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # 6. 插入 new_orders
    resp = send_sql(sock, f'INSERT INTO new_orders VALUES ({o_id}, {d_id}, {w_id});')
    if 'abort' in resp.lower() or 'error' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # 7. 对每个 item: 读取 item, 更新 stock, 插入 order_line
    total_amount = 0.0
    for idx, (ol_i_id, ol_quantity, ol_supply_w_id) in enumerate(items, 1):
        # 读取 item price
        resp = send_sql(sock, f'SELECT i_price, i_name FROM item WHERE i_id = {ol_i_id};')
        if 'abort' in resp.lower():
            send_sql(sock, 'ABORT;')
            return False

        # 读取并更新 stock
        resp = send_sql(sock, f'SELECT s_quantity, s_data FROM stock WHERE s_i_id = {ol_i_id} AND s_w_id = {ol_supply_w_id};')
        if 'abort' in resp.lower():
            send_sql(sock, 'ABORT;')
            return False

        resp = send_sql(sock, f'UPDATE stock SET s_quantity = s_quantity - {ol_quantity}, s_ytd = s_ytd + {ol_quantity}, s_order_cnt = s_order_cnt + 1 WHERE s_i_id = {ol_i_id} AND s_w_id = {ol_supply_w_id};')
        if 'abort' in resp.lower():
            send_sql(sock, 'ABORT;')
            return False

        # 插入 order_line
        ol_amount = ol_quantity * (idx * 1.5)
        resp = send_sql(sock, f"INSERT INTO order_line VALUES ({o_id}, {d_id}, {w_id}, {idx}, {ol_i_id}, {ol_supply_w_id}, '', {ol_quantity}, {ol_amount:.2f}, 'dist{d_id:02d}');")
        if 'abort' in resp.lower() or 'error' in resp.lower():
            send_sql(sock, 'ABORT;')
            return False

    resp = send_sql(sock, 'COMMIT;')
    return 'abort' not in resp.lower()

def payment_txn(sock, w_id, d_id, c_id, amount):
    """
    Payment 事务
    """
    send_sql(sock, 'BEGIN;')

    # 1. 更新 warehouse ytd
    resp = send_sql(sock, f'UPDATE warehouse SET w_ytd = w_ytd + {amount} WHERE w_id = {w_id};')
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # 2. 更新 district ytd
    resp = send_sql(sock, f'UPDATE district SET d_ytd = d_ytd + {amount} WHERE d_id = {d_id} AND d_w_id = {w_id};')
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # 3. 读取 customer
    resp = send_sql(sock, f'SELECT c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, c_zip, c_phone, c_since, c_credit, c_credit_lim, c_discount, c_balance FROM customer WHERE c_id = {c_id} AND c_d_id = {d_id} AND c_w_id = {w_id};')
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # 4. 更新 customer
    resp = send_sql(sock, f'UPDATE customer SET c_balance = c_balance - {amount}, c_ytd_payment = c_ytd_payment + {amount}, c_payment_cnt = c_payment_cnt + 1 WHERE c_id = {c_id} AND c_d_id = {d_id} AND c_w_id = {w_id};')
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # 5. 插入 history
    resp = send_sql(sock, f"INSERT INTO history VALUES ({c_id}, {d_id}, {w_id}, {d_id}, {w_id}, '2020-01-01', {amount}, 'data');")
    if 'abort' in resp.lower() or 'error' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    resp = send_sql(sock, 'COMMIT;')
    return 'abort' not in resp.lower()

def order_status_txn(sock, w_id, d_id, c_id):
    """
    OrderStatus 事务 (只读)
    """
    send_sql(sock, 'BEGIN;')

    # 1. 读取 customer
    resp = send_sql(sock, f'SELECT c_first, c_middle, c_last, c_balance FROM customer WHERE c_id = {c_id} AND c_d_id = {d_id} AND c_w_id = {w_id};')
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # 2. 读取最近订单
    resp = send_sql(sock, f'SELECT o_id, o_entry_d, o_carrier_id FROM orders WHERE o_w_id = {w_id} AND o_d_id = {d_id} AND o_c_id = {c_id} ORDER BY o_id DESC LIMIT 1;')
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # 3. 读取订单行
    resp = send_sql(sock, f'SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount FROM order_line WHERE ol_w_id = {w_id} AND ol_d_id = {d_id} AND ol_o_id = {c_id};')
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    resp = send_sql(sock, 'COMMIT;')
    return 'abort' not in resp.lower()

def delivery_txn(sock, w_id, carrier_id):
    """
    Delivery 事务
    """
    send_sql(sock, 'BEGIN;')

    for d_id in range(1, 11):
        # 1. 读取最小 new_order
        resp = send_sql(sock, f'SELECT no_o_id FROM new_orders WHERE no_w_id = {w_id} AND no_d_id = {d_id} ORDER BY no_o_id LIMIT 1;')
        if 'abort' in resp.lower():
            send_sql(sock, 'ABORT;')
            return False

        # 解析 o_id
        lines = resp.strip().split('\n')
        o_id = None
        for line in lines:
            p = line.strip().strip('|').strip()
            if p.isdigit():
                o_id = int(p)
                break

        if o_id is None:
            continue

        # 2. 删除 new_order
        resp = send_sql(sock, f'DELETE FROM new_orders WHERE no_o_id = {o_id} AND no_d_id = {d_id} AND no_w_id = {w_id};')
        if 'abort' in resp.lower():
            send_sql(sock, 'ABORT;')
            return False

        # 3. 更新 orders
        resp = send_sql(sock, f'UPDATE orders SET o_carrier_id = {carrier_id} WHERE o_id = {o_id} AND o_d_id = {d_id} AND o_w_id = {w_id};')
        if 'abort' in resp.lower():
            send_sql(sock, 'ABORT;')
            return False

        # 4. 更新 order_line
        resp = send_sql(sock, f"UPDATE order_line SET ol_delivery_d = '2020-01-01' WHERE ol_o_id = {o_id} AND ol_d_id = {d_id} AND ol_w_id = {w_id};")
        if 'abort' in resp.lower():
            send_sql(sock, 'ABORT;')
            return False

        # 5. 更新 customer (简化)
        resp = send_sql(sock, f'SELECT SUM(ol_amount) FROM order_line WHERE ol_o_id = {o_id} AND ol_d_id = {d_id} AND ol_w_id = {w_id};')
        if 'abort' in resp.lower():
            send_sql(sock, 'ABORT;')
            return False

    resp = send_sql(sock, 'COMMIT;')
    return 'abort' not in resp.lower()

def stock_level_txn(sock, w_id, d_id, threshold):
    """
    StockLevel 事务 (只读)
    """
    send_sql(sock, 'BEGIN;')

    # 1. 读取 district d_next_o_id
    resp = send_sql(sock, f'SELECT d_next_o_id FROM district WHERE d_id = {d_id} AND d_w_id = {w_id};')
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    # 2. 读取低库存数量 (简化)
    resp = send_sql(sock, f'SELECT COUNT(*) FROM stock WHERE s_w_id = {w_id} AND s_quantity < {threshold};')
    if 'abort' in resp.lower():
        send_sql(sock, 'ABORT;')
        return False

    resp = send_sql(sock, 'COMMIT;')
    return 'abort' not in resp.lower()

# ============================================================
# Worker 线程
# ============================================================
def tpcc_worker(wid, duration, results, num_items=100, num_customers=10):
    """TPC-C worker 线程"""
    try:
        sock = create_connection()
        new_orders = 0
        payments = 0
        order_status = 0
        delivery = 0
        stock_level = 0
        aborts = 0
        start = time.time()

        while time.time() - start < duration:
            w_id = 1
            d_id = (wid % 10) + 1
            c_id = random.randint(1, num_customers)

            # TPC-C 事务分布: NewOrder 45%, Payment 43%, OrderStatus 4%, Delivery 4%, StockLevel 4%
            txn_type = random.random()

            if txn_type < 0.45:
                # NewOrder
                ol_cnt = random.randint(5, 15)
                items = [(random.randint(1, num_items), random.randint(1, 10), w_id) for _ in range(ol_cnt)]
                if new_order_txn(sock, w_id, d_id, c_id, items):
                    new_orders += 1
                else:
                    aborts += 1
            elif txn_type < 0.88:
                # Payment
                amount = random.uniform(1.0, 5000.0)
                if payment_txn(sock, w_id, d_id, c_id, amount):
                    payments += 1
                else:
                    aborts += 1
            elif txn_type < 0.92:
                # OrderStatus
                if order_status_txn(sock, w_id, d_id, c_id):
                    order_status += 1
                else:
                    aborts += 1
            elif txn_type < 0.96:
                # Delivery
                if delivery_txn(sock, w_id, random.randint(1, 10)):
                    delivery += 1
                else:
                    aborts += 1
            else:
                # StockLevel
                if stock_level_txn(sock, w_id, d_id, random.randint(10, 50)):
                    stock_level += 1
                else:
                    aborts += 1

        sock.close()
        results.append((wid, new_orders, payments, order_status, delivery, stock_level, aborts))
    except Exception as e:
        results.append((wid, 0, 0, 0, 0, 0, str(e)))

# ============================================================
# 主测试流程
# ============================================================
def run_benchmark(num_workers=4, duration=30, use_csv=False, csv_dir=None, num_runs=3):
    """运行 TPC-C 基准测试"""
    db_name = "bench_tpcc_v2_db"

    print("=" * 60)
    print(f"TPC-C Benchmark (W=1, {num_workers} workers, {duration}s, {num_runs} runs)")
    print("=" * 60)

    all_tpmc = []
    all_throughput = []
    all_abort_rate = []

    for run in range(num_runs):
        # 每次运行重新创建数据库
        print(f"\n[Run {run + 1}/{num_runs}] Setting up database...")
        server = setup_and_load(db_name, use_csv=use_csv, csv_dir=csv_dir)

        print(f"[Run {run + 1}/{num_runs}] Running benchmark...")
        results = []
        threads = []

        start = time.time()
        for i in range(num_workers):
            t = threading.Thread(target=tpcc_worker, args=(i, duration, results))
            threads.append(t)
            t.start()

        for t in threads:
            t.join()
        elapsed = time.time() - start

        total_new_orders = sum(r[1] for r in results if isinstance(r[1], int))
        total_payments = sum(r[2] for r in results if isinstance(r[2], int))
        total_order_status = sum(r[3] for r in results if isinstance(r[3], int))
        total_delivery = sum(r[4] for r in results if isinstance(r[4], int))
        total_stock_level = sum(r[5] for r in results if isinstance(r[5], int))
        total_aborts = sum(r[6] for r in results if isinstance(r[6], int))
        total_txns = total_new_orders + total_payments + total_order_status + total_delivery + total_stock_level

        tpmc = total_new_orders / (elapsed / 60) if elapsed > 0 else 0
        throughput = total_txns / elapsed if elapsed > 0 else 0
        abort_rate = total_aborts / (total_txns + total_aborts) * 100 if (total_txns + total_aborts) > 0 else 0

        all_tpmc.append(tpmc)
        all_throughput.append(throughput)
        all_abort_rate.append(abort_rate)

        print(f"  NewOrder: {total_new_orders}, Payment: {total_payments}, "
              f"OrderStatus: {total_order_status}, Delivery: {total_delivery}, StockLevel: {total_stock_level}")
        print(f"  Aborts: {total_aborts}, tpmC: {tpmc:.1f}, Throughput: {throughput:.1f} txns/sec, AbortRate: {abort_rate:.1f}%")

        # 清理本次运行
        subprocess.run(['pkill', '-f', f'rmdb {db_name}'], capture_output=True)
        server.wait(timeout=3)
        subprocess.run(['rm', '-rf', db_name], capture_output=True)
        time.sleep(1)

    # 汇总
    avg_tpmc = sum(all_tpmc) / len(all_tpmc)
    avg_throughput = sum(all_throughput) / len(all_throughput)
    avg_abort_rate = sum(all_abort_rate) / len(all_abort_rate)
    median_tpmc = sorted(all_tpmc)[len(all_tpmc) // 2]

    print("\n" + "=" * 60)
    print(f"Results ({num_runs} runs)")
    print("=" * 60)
    print(f"  tpmC (avg):     {avg_tpmc:.1f}")
    print(f"  tpmC (median):  {median_tpmc:.1f}")
    print(f"  tpmC (all):     {[f'{x:.1f}' for x in all_tpmc]}")
    print(f"  Throughput:     {avg_throughput:.1f} txns/sec")
    print(f"  Abort rate:     {avg_abort_rate:.1f}%")
    print("=" * 60)

    return avg_tpmc, median_tpmc, avg_throughput, avg_abort_rate

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description='TPC-C Benchmark')
    parser.add_argument('-w', '--workers', type=int, default=4, help='Number of worker threads')
    parser.add_argument('-d', '--duration', type=int, default=30, help='Duration per run in seconds')
    parser.add_argument('-r', '--runs', type=int, default=3, help='Number of runs')
    parser.add_argument('--csv', type=str, default=None, help='CSV data directory')
    args = parser.parse_args()

    csv_dir = args.csv
    use_csv = csv_dir is not None and os.path.exists(csv_dir)

    run_benchmark(
        num_workers=args.workers,
        duration=args.duration,
        use_csv=use_csv,
        csv_dir=csv_dir,
        num_runs=args.runs
    )
