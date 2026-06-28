#!/usr/bin/env python3
"""TPC-C 性能测试脚本 - 对标线上评测方法

使用方法:
  python3 bench_tpcc_perf.py -w 8 --warehouses 2
  python3 bench_tpcc_perf.py -w 8 --warehouses 1 --warmup 10 --measure 30 --repeat 1  # 快速测试
  python3 bench_tpcc_perf.py -w 8 --warehouses 1 --warmup 30 --measure 360 --repeat 3  # 完整测试(对标线上)
"""

import argparse
import random
import socket
import subprocess
import threading
import time
from collections import defaultdict

PORT = 8765
DB_NAME = "bench_tpcc_perf_db"


def send_sql(sock, sql, timeout=30):
    sock.settimeout(timeout)
    sock.sendall((sql + "\0").encode())
    chunks = []
    while True:
        chunk = sock.recv(1 << 20)
        if not chunk:
            break
        chunks.append(chunk)
        if b"\0" in chunk:
            break
    return b"".join(chunks).decode(errors="replace").replace("\0", "")


def connect(snapshot=True):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(30)
    sock.connect(("127.0.0.1", PORT))
    if snapshot:
        send_sql(sock, "SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;")
    send_sql(sock, "set output_file off")
    return sock


def is_bad_response(resp):
    lower = resp.lower()
    return "abort" in lower or "error" in lower or "failure" in lower


def query_scalar(sock, sql):
    resp = send_sql(sock, sql)
    for line in resp.splitlines():
        line = line.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) == 1:
            cell = cells[0]
            if cell == "" or any(c.isalpha() for c in cell):
                continue
            try:
                if "." in cell:
                    return float(cell)
                return int(cell)
            except ValueError:
                continue
    return None


def start_server():
    subprocess.run(["pkill", "-f", f"rmdb {DB_NAME}"], capture_output=True)
    subprocess.run(["rm", "-rf", DB_NAME], capture_output=True)
    server = subprocess.Popen(
        ["./build/bin/rmdb", DB_NAME],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(1.5)
    if server.poll() is not None:
        raise RuntimeError("rmdb server failed to start")
    return server


# ==================== TPC-C 表定义 ====================

TABLES = [
    "CREATE TABLE warehouse (w_id int, w_name char(10), w_street_1 char(20), w_street_2 char(20), w_city char(20), w_state char(2), w_zip char(9), w_tax float, w_ytd float);",
    "CREATE TABLE district (d_id int, d_w_id int, d_name char(10), d_street_1 char(20), d_street_2 char(20), d_city char(20), d_state char(2), d_zip char(9), d_tax float, d_ytd float, d_next_o_id int);",
    "CREATE TABLE customer (c_id int, c_d_id int, c_w_id int, c_first char(16), c_middle char(2), c_last char(16), c_street_1 char(20), c_street_2 char(20), c_city char(20), c_state char(2), c_zip char(9), c_phone char(16), c_since char(30), c_credit char(2), c_credit_lim float, c_discount float, c_balance float, c_ytd_payment float, c_payment_cnt int, c_delivery_cnt int, c_data char(50));",
    "CREATE TABLE history (h_c_id int, h_c_d_id int, h_c_w_id int, h_d_id int, h_w_id int, h_date char(30), h_amount float, h_data char(24));",
    "CREATE TABLE orders (o_id int, o_d_id int, o_w_id int, o_c_id int, o_entry_d char(30), o_carrier_id int, o_ol_cnt int, o_all_local int);",
    "CREATE TABLE new_orders (no_o_id int, no_d_id int, no_w_id int);",
    "CREATE TABLE order_line (ol_o_id int, ol_d_id int, ol_w_id int, ol_number int, ol_i_id int, ol_supply_w_id int, ol_delivery_d char(30), ol_quantity int, ol_amount float, ol_dist_info char(24));",
    "CREATE TABLE item (i_id int, i_im_id int, i_name char(24), i_price float, i_data char(50));",
    "CREATE TABLE stock (s_i_id int, s_w_id int, s_quantity int, s_dist_01 char(24), s_dist_02 char(24), s_dist_03 char(24), s_dist_04 char(24), s_dist_05 char(24), s_dist_06 char(24), s_dist_07 char(24), s_dist_08 char(24), s_dist_09 char(24), s_dist_10 char(24), s_ytd int, s_order_cnt int, s_remote_cnt int, s_data char(50));",
]

INDEXES = [
    "CREATE INDEX warehouse (w_id);",
    "CREATE INDEX district (d_w_id, d_id);",
    "CREATE INDEX customer (c_w_id, c_d_id, c_id);",
    "CREATE INDEX orders (o_w_id, o_d_id, o_id);",
    "CREATE INDEX new_orders (no_w_id, no_d_id, no_o_id);",
    "CREATE INDEX order_line (ol_w_id, ol_d_id, ol_o_id, ol_number);",
    "CREATE INDEX item (i_id);",
    "CREATE INDEX stock (s_w_id, s_i_id);",
]


def setup_database(sock, warehouses, items, customers_per_dist, initial_orders, new_order_start):
    """建表 + 建索引 + 加载数据"""
    for sql in TABLES:
        resp = send_sql(sock, sql)
        if is_bad_response(resp):
            raise RuntimeError(f"DDL failed: {sql}: {resp}")
    for sql in INDEXES:
        resp = send_sql(sock, sql)
        if is_bad_response(resp):
            raise RuntimeError(f"Index failed: {sql}: {resp}")

    # warehouse
    for w in range(1, warehouses + 1):
        send_sql(sock, f"INSERT INTO warehouse VALUES ({w}, 'wname', 'st1', 'st2', 'city', 'ST', '123456789', 0.10, 300000.00);")

    # item (10万)
    for i in range(1, items + 1):
        send_sql(sock, f"INSERT INTO item VALUES ({i}, {i}, 'item{i}', {i * 0.5:.2f}, 'data');")

    # stock (W * 10万)
    for w in range(1, warehouses + 1):
        for i in range(1, items + 1):
            send_sql(sock, f"INSERT INTO stock VALUES ({i}, {w}, 100, 'd01','d02','d03','d04','d05','d06','d07','d08','d09','d10', 0, 0, 0, 'data');")

    # district, customer, history, orders, new_orders, order_line
    for w in range(1, warehouses + 1):
        for d in range(1, 11):
            send_sql(sock, f"INSERT INTO district VALUES ({d}, {w}, 'dname', 'st1', 'st2', 'city', 'ST', '123456789', 0.10, 30000.00, {initial_orders + 1});")
            for c in range(1, customers_per_dist + 1):
                send_sql(sock, f"INSERT INTO customer VALUES ({c}, {d}, {w}, 'first', 'OE', 'last', 'st1', 'st2', 'city', 'ST', '123456789', 'phone', '2020-01-01', 'GC', 50000.00, 0.10, -10.00, 10.00, 1, 0, 'data');")
                send_sql(sock, f"INSERT INTO history VALUES ({c}, {d}, {w}, {d}, {w}, '2020-01-01', 10.00, 'data');")
            for o in range(1, initial_orders + 1):
                c_id = ((o - 1) % customers_per_dist) + 1
                ol_cnt = 5 + (o % 11)
                send_sql(sock, f"INSERT INTO orders VALUES ({o}, {d}, {w}, {c_id}, '2020-01-01', 0, {ol_cnt}, 1);")
                if o >= new_order_start:
                    send_sql(sock, f"INSERT INTO new_orders VALUES ({o}, {d}, {w});")
                for ol in range(1, ol_cnt + 1):
                    item = ((o + ol) % items) + 1
                    send_sql(sock, f"INSERT INTO order_line VALUES ({o}, {d}, {w}, {ol}, {item}, {w}, '', 5, {ol * 1.25:.2f}, 'dist');")


# ==================== TPC-C 事务 ====================

def new_order_txn(sock, stats, lock, rng, warehouses, items, customers_per_dist):
    w_id = rng.randint(1, warehouses)
    d_id = rng.randint(1, 10)
    c_id = rng.randint(1, customers_per_dist)
    ol_cnt = rng.randint(5, 15)
    txns = [(rng.randint(1, items), rng.randint(1, 10)) for _ in range(ol_cnt)]

    send_sql(sock, "BEGIN;")
    o_id = query_scalar(sock, f"SELECT d_next_o_id FROM district WHERE d_id={d_id} AND d_w_id={w_id};")
    if o_id is None:
        send_sql(sock, "ABORT;")
        return False

    for sql in [
        f"UPDATE district SET d_next_o_id=d_next_o_id+1 WHERE d_id={d_id} AND d_w_id={w_id};",
        f"INSERT INTO orders VALUES ({o_id}, {d_id}, {w_id}, {c_id}, '2020-01-01', 0, {ol_cnt}, 1);",
        f"INSERT INTO new_orders VALUES ({o_id}, {d_id}, {w_id});",
    ]:
        if is_bad_response(send_sql(sock, sql)):
            send_sql(sock, "ABORT;")
            return False

    for idx, (item, qty) in enumerate(txns, 1):
        if is_bad_response(send_sql(sock, f"SELECT i_price, i_name FROM item WHERE i_id={item};")):
            send_sql(sock, "ABORT;")
            return False
        if is_bad_response(send_sql(sock, f"SELECT s_quantity, s_data FROM stock WHERE s_i_id={item} AND s_w_id={w_id};")):
            send_sql(sock, "ABORT;")
            return False
        if is_bad_response(send_sql(sock, f"UPDATE stock SET s_quantity=s_quantity-{qty}, s_ytd=s_ytd+{qty}, s_order_cnt=s_order_cnt+1 WHERE s_i_id={item} AND s_w_id={w_id};")):
            send_sql(sock, "ABORT;")
            return False
        if is_bad_response(send_sql(sock, f"INSERT INTO order_line VALUES ({o_id}, {d_id}, {w_id}, {idx}, {item}, {w_id}, '', {qty}, {qty * 1.25:.2f}, 'dist');")):
            send_sql(sock, "ABORT;")
            return False

    resp = send_sql(sock, "COMMIT;")
    if is_bad_response(resp):
        return False
    with lock:
        stats["new_order"] += 1
    return True


def payment_txn(sock, rng, warehouses, customers_per_dist):
    w_id = rng.randint(1, warehouses)
    d_id = rng.randint(1, 10)
    c_id = rng.randint(1, customers_per_dist)
    amount = rng.randint(1, 100)
    send_sql(sock, "BEGIN;")
    for sql in [
        f"UPDATE warehouse SET w_ytd=w_ytd+{amount} WHERE w_id={w_id};",
        f"UPDATE district SET d_ytd=d_ytd+{amount} WHERE d_id={d_id} AND d_w_id={w_id};",
        f"UPDATE customer SET c_balance=c_balance-{amount}, c_ytd_payment=c_ytd_payment+{amount}, c_payment_cnt=c_payment_cnt+1 WHERE c_id={c_id} AND c_d_id={d_id} AND c_w_id={w_id};",
        f"INSERT INTO history VALUES ({c_id}, {d_id}, {w_id}, {d_id}, {w_id}, '2020-01-01', {amount}, 'data');",
    ]:
        if is_bad_response(send_sql(sock, sql)):
            send_sql(sock, "ABORT;")
            return False
    return not is_bad_response(send_sql(sock, "COMMIT;"))


def order_status_txn(sock, rng, warehouses, customers_per_dist):
    w_id = rng.randint(1, warehouses)
    d_id = rng.randint(1, 10)
    c_id = rng.randint(1, customers_per_dist)
    send_sql(sock, "BEGIN;")
    for sql in [
        f"SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE c_w_id={w_id} AND c_d_id={d_id} AND c_id={c_id};",
        f"SELECT o_id, o_entry_d, o_carrier_id FROM orders WHERE o_w_id={w_id} AND o_d_id={d_id} AND o_c_id={c_id} ORDER BY o_id DESC LIMIT 1;",
    ]:
        if is_bad_response(send_sql(sock, sql)):
            send_sql(sock, "ABORT;")
            return False
    return not is_bad_response(send_sql(sock, "COMMIT;"))


def delivery_txn(sock, rng, warehouses):
    w_id = rng.randint(1, warehouses)
    send_sql(sock, "BEGIN;")
    for d_id in range(1, 11):
        o_id = query_scalar(sock, f"SELECT MIN(no_o_id) as min_o_id FROM new_orders WHERE no_d_id={d_id} AND no_w_id={w_id};")
        if o_id is None:
            continue
        c_id = query_scalar(sock, f"SELECT o_c_id FROM orders WHERE o_id={o_id} AND o_d_id={d_id} AND o_w_id={w_id};")
        if c_id is None:
            send_sql(sock, "ABORT;")
            return False
        amount = query_scalar(sock, f"SELECT SUM(ol_amount) as sum_amount FROM order_line WHERE ol_o_id={o_id} AND ol_d_id={d_id};")
        if amount is None:
            amount = 0
        for sql in [
            f"DELETE FROM new_orders WHERE no_o_id={o_id} AND no_d_id={d_id} AND no_w_id={w_id};",
            f"UPDATE orders SET o_carrier_id=1 WHERE o_id={o_id} AND o_d_id={d_id} AND o_w_id={w_id};",
            f"UPDATE order_line SET ol_delivery_d='2020-01-01' WHERE ol_o_id={o_id} AND ol_d_id={d_id} AND ol_w_id={w_id};",
            f"UPDATE customer SET c_balance=c_balance+{amount}, c_delivery_cnt=c_delivery_cnt+1 WHERE c_id={c_id} AND c_d_id={d_id} AND c_w_id={w_id};",
        ]:
            if is_bad_response(send_sql(sock, sql)):
                send_sql(sock, "ABORT;")
                return False
    return not is_bad_response(send_sql(sock, "COMMIT;"))


def stock_level_txn(sock, rng, warehouses):
    w_id = rng.randint(1, warehouses)
    d_id = rng.randint(1, 10)
    threshold = rng.randint(10, 20)
    send_sql(sock, "BEGIN;")
    d_next = query_scalar(sock, f"SELECT d_next_o_id FROM district WHERE d_id={d_id} AND d_w_id={w_id};")
    if d_next is None:
        send_sql(sock, "ABORT;")
        return False
    low = max(1, d_next - 20)
    send_sql(sock, f"SELECT DISTINCT ol_i_id FROM order_line WHERE ol_w_id={w_id} AND ol_d_id={d_id} AND ol_o_id >= {low} AND ol_o_id < {d_next};")
    return not is_bad_response(send_sql(sock, "COMMIT;"))


# ==================== Worker ====================

def worker(worker_id, duration, stats, lock, warehouses, items, customers_per_dist):
    """每个 worker 循环执行 TPC-C 事务直到时间到"""
    sock = connect(snapshot=True)
    rng = random.Random(worker_id * 12345 + 67890)
    local_stats = defaultdict(int)
    end_time = time.time() + duration

    try:
        while time.time() < end_time:
            r = rng.random()
            if r < 0.45:
                ok = new_order_txn(sock, local_stats, lock, rng, warehouses, items, customers_per_dist)
                local_stats["new_order_attempt"] += 1
            elif r < 0.88:
                ok = payment_txn(sock, rng, warehouses, customers_per_dist)
            elif r < 0.92:
                ok = order_status_txn(sock, rng, warehouses, customers_per_dist)
            elif r < 0.96:
                ok = delivery_txn(sock, rng, warehouses)
            else:
                ok = stock_level_txn(sock, rng, warehouses)
            if not ok:
                local_stats["abort"] += 1
    finally:
        sock.close()
        with lock:
            for k, v in local_stats.items():
                stats[k] += v


# ==================== 一致性检查 ====================

def validate_consistency(sock, warehouses):
    """压测后验证数据一致性"""
    failures = []

    for w in range(1, warehouses + 1):
        for d in range(1, 11):
            d_next = query_scalar(sock, f"SELECT d_next_o_id FROM district WHERE d_w_id={w} AND d_id={d};")
            max_o = query_scalar(sock, f"SELECT MAX(o_id) FROM orders WHERE o_w_id={w} AND o_d_id={d};")
            max_no = query_scalar(sock, f"SELECT MAX(no_o_id) FROM new_orders WHERE no_w_id={w} AND no_d_id={d};")
            count_no = query_scalar(sock, f"SELECT COUNT(*) FROM new_orders WHERE no_w_id={w} AND no_d_id={d};")

            if d_next is not None and max_o is not None and d_next != max_o + 1:
                failures.append(f"d_next mismatch ({w},{d}): d_next={d_next}, max_o={max_o}")
            if max_no is not None and max_o is not None and max_no != max_o:
                failures.append(f"max_no mismatch ({w},{d}): max_no={max_no}, max_o={max_o}")

    return failures


# ==================== 主流程 ====================

def run_one_round(args, round_num):
    """运行一轮：加载数据 + 预热 + 测量"""
    print(f"\n{'='*60}")
    print(f"Round {round_num + 1}/{args.repeat}")
    print(f"{'='*60}")

    server = start_server()

    try:
        # 连接并加载数据
        sock = connect(snapshot=False)
        print(f"Loading data: {args.warehouses} warehouses, {args.items} items, {args.customers} customers/dist...")
        t0 = time.time()
        setup_database(sock, args.warehouses, args.items, args.customers, args.initial_orders, args.new_order_start)
        load_time = time.time() - t0
        print(f"Data loaded in {load_time:.1f}s")
        sock.close()

        # 预热
        if args.warmup > 0:
            print(f"Warming up ({args.warmup}s)...")
            stats = defaultdict(int)
            lock = threading.Lock()
            threads = [threading.Thread(target=worker, args=(i, args.warmup, stats, lock, args.warehouses, args.items, args.customers)) for i in range(args.workers)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            print(f"Warmup: {stats.get('new_order', 0)} new_orders, {stats.get('abort', 0)} aborts")

        # 正式测量
        print(f"Measuring ({args.measure}s)...")
        stats = defaultdict(int)
        lock = threading.Lock()
        t0 = time.time()
        threads = [threading.Thread(target=worker, args=(i, args.measure, stats, lock, args.warehouses, args.items, args.customers)) for i in range(args.workers)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        elapsed = time.time() - t0

        new_order = stats.get("new_order", 0)
        aborts = stats.get("abort", 0)
        tpmC = new_order / (elapsed / 60)

        print(f"Results: {new_order} new_orders in {elapsed:.1f}s = {tpmC:.1f} tpmC, {aborts} aborts")

        # 一致性检查
        print("Running consistency check...")
        sock = connect(snapshot=False)
        failures = validate_consistency(sock, args.warehouses)
        sock.close()

        if failures:
            print(f"CONSISTENCY FAILED: {failures[0]}")
            return {"tpmC": 0, "new_order": 0, "aborts": aborts, "consistency": "FAILED", "error": failures[0]}
        else:
            print("CONSISTENCY PASSED")
            return {"tpmC": tpmC, "new_order": new_order, "aborts": aborts, "consistency": "PASSED"}

    finally:
        subprocess.run(["pkill", "-f", f"rmdb {DB_NAME}"], capture_output=True)
        try:
            server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
        subprocess.run(["rm", "-rf", DB_NAME], capture_output=True)


def main():
    parser = argparse.ArgumentParser(description="TPC-C Performance Benchmark")
    parser.add_argument("-w", "--workers", type=int, default=4, help="并发 worker 数")
    parser.add_argument("--warehouses", type=int, default=1, help="仓库数")
    parser.add_argument("--items", type=int, default=100000, help="商品数")
    parser.add_argument("--customers", type=int, default=3000, help="每区客户数")
    parser.add_argument("--initial-orders", type=int, default=3000, help="每区初始订单数")
    parser.add_argument("--new-order-start", type=int, default=2101, help="new_orders 起始 o_id")
    parser.add_argument("--warmup", type=int, default=30, help="预热秒数")
    parser.add_argument("--measure", type=int, default=360, help="测量秒数")
    parser.add_argument("--repeat", type=int, default=3, help="重复轮数")
    args = parser.parse_args()

    print(f"TPC-C Performance Test")
    print(f"  Workers: {args.workers}")
    print(f"  Warehouses: {args.warehouses}")
    print(f"  Items: {args.items}")
    print(f"  Customers/dist: {args.customers}")
    print(f"  Warmup: {args.warmup}s, Measure: {args.measure}s, Repeat: {args.repeat}")

    results = []
    for i in range(args.repeat):
        r = run_one_round(args, i)
        results.append(r)

    # 中位数
    tpmcs = sorted([r["tpmC"] for r in results])
    median_tpmc = tpmcs[len(tpmcs) // 2]
    all_passed = all(r["consistency"] == "PASSED" for r in results)

    print(f"\n{'='*60}")
    print(f"FINAL RESULTS")
    print(f"{'='*60}")
    for i, r in enumerate(results):
        print(f"  Round {i+1}: tpmC={r['tpmC']:.1f}, new_order={r['new_order']}, aborts={r['aborts']}, consistency={r['consistency']}")
    print(f"  Median tpmC: {median_tpmc:.1f}")
    print(f"  All consistency: {'PASSED' if all_passed else 'FAILED'}")

    return 0 if all_passed else 1


if __name__ == "__main__":
    exit(main())
