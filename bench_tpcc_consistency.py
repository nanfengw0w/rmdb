#!/usr/bin/env python3
"""Run a small TPC-C style workload and validate post-transaction consistency.

This script is intentionally stricter than the old short benchmarks: after the
workload it runs the same classes of aggregate checks described in
`数据一致性检验规则.pdf`. It is meant to catch read-path changes that pass
functional tests but break Phase 3 consistency validation.
"""

import argparse
import random
import re
import socket
import subprocess
import threading
import time
from collections import defaultdict


PORT = 8765
DB_NAME = "bench_tpcc_consistency_db"

WAREHOUSES = 1
DISTRICTS = 10
CUSTOMERS_PER_DIST = 30
ITEMS = 200
INITIAL_ORDERS_PER_DIST = 30
INITIAL_NEW_ORDER_START = 21


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


def parse_cells(resp):
    rows = []
    for line in resp.splitlines():
        line = line.strip()
        if not line.startswith("|"):
            continue
        cells = [part.strip() for part in line.strip("|").split("|")]
        if not cells:
            continue
        rows.append(cells)
    return rows


def parse_table(resp):
    rows = parse_cells(resp)
    if not rows:
        return []
    header = rows[0]
    out = []
    for row in rows[1:]:
        if len(row) != len(header):
            continue
        out.append(dict(zip(header, row)))
    return out


def to_int(value):
    if value is None or value == "" or value.upper() == "NULL":
        return None
    return int(float(value))


def to_float(value):
    if value is None or value == "" or value.upper() == "NULL":
        return None
    return float(value)


def scalar(resp):
    for cells in parse_cells(resp):
        if len(cells) != 1:
            continue
        cell = cells[0]
        if cell == "" or re.search(r"[A-Za-z_]", cell):
            continue
        try:
            if "." in cell:
                return float(cell)
            return int(cell)
        except ValueError:
            continue
    return None


def query_scalar(sock, sql):
    return scalar(send_sql(sock, sql))


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
    "CREATE INDEX district (d_id, d_w_id);",
    "CREATE INDEX customer (c_id, c_d_id, c_w_id);",
    "CREATE INDEX orders (o_id, o_d_id, o_w_id);",
    "CREATE INDEX new_orders (no_o_id, no_d_id, no_w_id);",
    "CREATE INDEX order_line (ol_o_id, ol_d_id, ol_w_id, ol_number);",
    "CREATE INDEX item (i_id);",
    "CREATE INDEX stock (s_i_id, s_w_id);",
]

TPCC_INDEXES = [
    "CREATE INDEX warehouse (w_id);",
    "CREATE INDEX district (d_w_id, d_id);",
    "CREATE INDEX customer (c_w_id, c_d_id, c_id);",
    "CREATE INDEX orders (o_w_id, o_d_id, o_id);",
    "CREATE INDEX new_orders (no_w_id, no_d_id, no_o_id);",
    "CREATE INDEX order_line (ol_w_id, ol_d_id, ol_o_id, ol_number);",
    "CREATE INDEX item (i_id);",
    "CREATE INDEX stock (s_w_id, s_i_id);",
]


def start_server():
    subprocess.run(["pkill", "-f", f"rmdb {DB_NAME}"], capture_output=True)
    subprocess.run(["rm", "-rf", DB_NAME], capture_output=True)
    server = subprocess.Popen(["./build/bin/rmdb", DB_NAME],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    time.sleep(1.5)
    if server.poll() is not None:
        raise RuntimeError("rmdb server failed to start")
    return server


def setup_database(index_order):
    sock = connect()
    for sql in TABLES:
        resp = send_sql(sock, sql)
        if is_bad_response(resp):
            raise RuntimeError(f"failed table DDL: {sql}: {resp}")
    indexes = TPCC_INDEXES if index_order == "tpcc" else INDEXES
    for sql in indexes:
        resp = send_sql(sock, sql)
        if is_bad_response(resp):
            raise RuntimeError(f"failed index DDL: {sql}: {resp}")

    for w_id in range(1, WAREHOUSES + 1):
        send_sql(sock, f"INSERT INTO warehouse VALUES ({w_id}, 'wname', 'st1', 'st2', 'city', 'ST', '123456789', 0.10, 300000.00);")
    for i_id in range(1, ITEMS + 1):
        send_sql(sock, f"INSERT INTO item VALUES ({i_id}, {i_id}, 'item{i_id}', {i_id * 0.5:.2f}, 'data');")
    for w_id in range(1, WAREHOUSES + 1):
        for i_id in range(1, ITEMS + 1):
            send_sql(sock, f"INSERT INTO stock VALUES ({i_id}, {w_id}, 100, 'd01', 'd02', 'd03', 'd04', 'd05', 'd06', 'd07', 'd08', 'd09', 'd10', 0, 0, 0, 'data');")

    for w_id in range(1, WAREHOUSES + 1):
        for d_id in range(1, DISTRICTS + 1):
            send_sql(sock, f"INSERT INTO district VALUES ({d_id}, {w_id}, 'dname', 'dst1', 'dst2', 'city', 'ST', '123456789', 0.10, 30000.00, {INITIAL_ORDERS_PER_DIST + 1});")
            for c_id in range(1, CUSTOMERS_PER_DIST + 1):
                send_sql(sock, f"INSERT INTO customer VALUES ({c_id}, {d_id}, {w_id}, 'first', 'OE', 'last', 'st1', 'st2', 'city', 'ST', '123456789', 'phone', '2020-01-01', 'GC', 50000.00, 0.10, -10.00, 10.00, 1, 0, 'data');")
                send_sql(sock, f"INSERT INTO history VALUES ({c_id}, {d_id}, {w_id}, {d_id}, {w_id}, '2020-01-01', 10.00, 'data');")
            for o_id in range(1, INITIAL_ORDERS_PER_DIST + 1):
                c_id = ((o_id - 1) % CUSTOMERS_PER_DIST) + 1
                ol_cnt = 5 + (o_id % 5)
                send_sql(sock, f"INSERT INTO orders VALUES ({o_id}, {d_id}, {w_id}, {c_id}, '2020-01-01', 0, {ol_cnt}, 1);")
                if o_id >= INITIAL_NEW_ORDER_START:
                    send_sql(sock, f"INSERT INTO new_orders VALUES ({o_id}, {d_id}, {w_id});")
                for ol_no in range(1, ol_cnt + 1):
                    item = ((o_id + ol_no) % ITEMS) + 1
                    send_sql(sock, f"INSERT INTO order_line VALUES ({o_id}, {d_id}, {w_id}, {ol_no}, {item}, {w_id}, '', 5, {ol_no * 1.25:.2f}, 'dist');")
    sock.close()


def new_order_txn(sock, stats, lock, rng):
    w_id = rng.randint(1, WAREHOUSES)
    d_id = rng.randint(1, DISTRICTS)
    c_id = rng.randint(1, CUSTOMERS_PER_DIST)
    ol_cnt = rng.randint(5, 10)
    items = [(rng.randint(1, ITEMS), rng.randint(1, 10)) for _ in range(ol_cnt)]

    send_sql(sock, "BEGIN;")
    o_id = query_scalar(sock, f"SELECT d_next_o_id FROM district WHERE d_id={d_id} AND d_w_id={w_id};")
    if o_id is None:
        send_sql(sock, "ABORT;")
        return False

    steps = [
        f"UPDATE district SET d_next_o_id=d_next_o_id+1 WHERE d_id={d_id} AND d_w_id={w_id};",
        f"INSERT INTO orders VALUES ({o_id}, {d_id}, {w_id}, {c_id}, '2020-01-01', 0, {ol_cnt}, 1);",
        f"INSERT INTO new_orders VALUES ({o_id}, {d_id}, {w_id});",
    ]
    for sql in steps:
        if is_bad_response(send_sql(sock, sql)):
            send_sql(sock, "ABORT;")
            return False

    for idx, (item, qty) in enumerate(items, 1):
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
        stats["new_order_total"] += 1
        stats["new_order_by_dist"][(w_id, d_id)] += 1
    return True


def delivery_txn(sock, rng):
    w_id = rng.randint(1, WAREHOUSES)
    carrier_id = 1
    send_sql(sock, "BEGIN;")
    for d_id in range(1, DISTRICTS + 1):
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
        steps = [
            f"DELETE FROM new_orders WHERE no_o_id={o_id} AND no_d_id={d_id} AND no_w_id={w_id};",
            f"UPDATE orders SET o_carrier_id={carrier_id} WHERE o_id={o_id} AND o_d_id={d_id} AND o_w_id={w_id};",
            f"UPDATE order_line SET ol_delivery_d='2020-01-01' WHERE ol_o_id={o_id} AND ol_d_id={d_id} AND ol_w_id={w_id};",
            f"UPDATE customer SET c_balance=c_balance+{amount}, c_delivery_cnt=c_delivery_cnt+1 WHERE c_id={c_id} AND c_d_id={d_id} AND c_w_id={w_id};",
        ]
        for sql in steps:
            if is_bad_response(send_sql(sock, sql)):
                send_sql(sock, "ABORT;")
                return False
    return not is_bad_response(send_sql(sock, "COMMIT;"))


def payment_txn(sock, rng):
    w_id = rng.randint(1, WAREHOUSES)
    d_id = rng.randint(1, DISTRICTS)
    c_id = rng.randint(1, CUSTOMERS_PER_DIST)
    amount = rng.randint(1, 100)
    send_sql(sock, "BEGIN;")
    steps = [
        f"UPDATE warehouse SET w_ytd=w_ytd+{amount} WHERE w_id={w_id};",
        f"UPDATE district SET d_ytd=d_ytd+{amount} WHERE d_id={d_id} AND d_w_id={w_id};",
        f"UPDATE customer SET c_balance=c_balance-{amount}, c_ytd_payment=c_ytd_payment+{amount}, c_payment_cnt=c_payment_cnt+1 WHERE c_id={c_id} AND c_d_id={d_id} AND c_w_id={w_id};",
        f"INSERT INTO history VALUES ({c_id}, {d_id}, {w_id}, {d_id}, {w_id}, '2020-01-01', {amount}, 'data');",
    ]
    for sql in steps:
        if is_bad_response(send_sql(sock, sql)):
            send_sql(sock, "ABORT;")
            return False
    return not is_bad_response(send_sql(sock, "COMMIT;"))


def order_status_txn(sock, rng):
    w_id = rng.randint(1, WAREHOUSES)
    d_id = rng.randint(1, DISTRICTS)
    c_id = rng.randint(1, CUSTOMERS_PER_DIST)
    send_sql(sock, "BEGIN;")
    queries = [
        f"SELECT COUNT(c_id) as count_c_id FROM customer WHERE c_w_id={w_id} AND c_d_id={d_id} AND c_last='last';",
        f"SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE c_w_id={w_id} AND c_d_id={d_id} AND c_last='last' ORDER BY c_first;",
        f"SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE c_w_id={w_id} AND c_d_id={d_id} AND c_id={c_id};",
        f"SELECT o_id, o_entry_d, o_carrier_id FROM orders WHERE o_w_id={w_id} AND o_d_id={d_id} AND o_c_id={c_id} ORDER BY o_id DESC LIMIT 1;",
        f"SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line WHERE ol_w_id={w_id} AND ol_d_id={d_id} AND ol_o_id={c_id};",
    ]
    for sql in queries:
        if is_bad_response(send_sql(sock, sql)):
            send_sql(sock, "ABORT;")
            return False
    return not is_bad_response(send_sql(sock, "COMMIT;"))


def stock_level_txn(sock, rng):
    w_id = rng.randint(1, WAREHOUSES)
    d_id = rng.randint(1, DISTRICTS)
    level = rng.randint(20, 90)
    send_sql(sock, "BEGIN;")
    d_next = query_scalar(sock, f"SELECT d_next_o_id FROM district WHERE d_id={d_id} AND d_w_id={w_id};")
    if d_next is None:
        send_sql(sock, "ABORT;")
        return False
    if is_bad_response(send_sql(sock, f"SELECT ol_i_id FROM order_line WHERE ol_w_id={w_id} AND ol_d_id={d_id} AND ol_o_id<{d_next} AND ol_o_id>={max(1, d_next - 20)};")):
        send_sql(sock, "ABORT;")
        return False
    item = rng.randint(1, ITEMS)
    if is_bad_response(send_sql(sock, f"SELECT COUNT(*) as count_stock FROM stock WHERE s_w_id={w_id} AND s_i_id={item} AND s_quantity<{level};")):
        send_sql(sock, "ABORT;")
        return False
    return not is_bad_response(send_sql(sock, "COMMIT;"))


def worker(worker_id, duration, stats, lock):
    rng = random.Random(20260627 + worker_id)
    sock = connect()
    end = time.time() + duration
    local = {"new_order": 0, "payment": 0, "order_status": 0, "delivery": 0, "stock_level": 0, "abort": 0}
    try:
        while time.time() < end:
            r = rng.random()
            if r < 0.45:
                ok = new_order_txn(sock, stats, lock, rng)
                local["new_order" if ok else "abort"] += 1
            elif r < 0.88:
                ok = payment_txn(sock, rng)
                local["payment" if ok else "abort"] += 1
            elif r < 0.92:
                ok = order_status_txn(sock, rng)
                local["order_status" if ok else "abort"] += 1
            elif r < 0.96:
                ok = delivery_txn(sock, rng)
                local["delivery" if ok else "abort"] += 1
            else:
                ok = stock_level_txn(sock, rng)
                local["stock_level" if ok else "abort"] += 1
    finally:
        sock.close()
        with lock:
            for k, v in local.items():
                stats[k] += v


def table_snapshot(sock, table):
    resp = send_sql(sock, f"SELECT * FROM {table};", timeout=120)
    if is_bad_response(resp):
        raise RuntimeError(f"snapshot query failed for {table}: {resp}")
    return parse_table(resp)


def build_snapshot_model(sock):
    model = {
        "district_next": {},
        "orders": defaultdict(list),
        "order_line_count": defaultdict(int),
        "new_orders": defaultdict(list),
        "stock": {},
        "new_order_stock_count": defaultdict(int),
        "new_order_stock_qty": defaultdict(int),
    }

    stock_keys = set()

    for w_id in range(1, WAREHOUSES + 1):
        for d_id in range(1, DISTRICTS + 1):
            key = (w_id, d_id)
            d_next = query_scalar(sock, f"SELECT d_next_o_id FROM district WHERE d_w_id={w_id} AND d_id={d_id};")
            model["district_next"][key] = d_next

    for w_id in range(1, WAREHOUSES + 1):
        for d_id in range(1, DISTRICTS + 1):
            key = (w_id, d_id)
            order_rows = parse_table(send_sql(
                sock,
                f"SELECT o_id, o_d_id, o_w_id, o_ol_cnt FROM orders WHERE o_w_id={w_id} AND o_d_id={d_id};",
                timeout=120))
            for row in order_rows:
                o_id = to_int(row["o_id"])
                ol_cnt = to_int(row["o_ol_cnt"])
                model["orders"][key].append((o_id, ol_cnt))

                ol_rows = parse_table(send_sql(
                    sock,
                    f"SELECT ol_o_id, ol_d_id, ol_w_id, ol_i_id, ol_supply_w_id, ol_quantity "
                    f"FROM order_line WHERE ol_w_id={w_id} AND ol_d_id={d_id} AND ol_o_id={o_id};",
                    timeout=120))
                model["order_line_count"][key] += len(ol_rows)
                if o_id is not None and o_id > INITIAL_ORDERS_PER_DIST:
                    for ol_row in ol_rows:
                        stock_key = (to_int(ol_row["ol_supply_w_id"]), to_int(ol_row["ol_i_id"]))
                        stock_keys.add(stock_key)
                        model["new_order_stock_count"][stock_key] += 1
                        model["new_order_stock_qty"][stock_key] += to_int(ol_row["ol_quantity"]) or 0

            no_rows = parse_table(send_sql(
                sock,
                f"SELECT no_o_id, no_d_id, no_w_id FROM new_orders WHERE no_w_id={w_id} AND no_d_id={d_id};",
                timeout=120))
            for row in no_rows:
                model["new_orders"][key].append(to_int(row["no_o_id"]))

    for w_id, i_id in stock_keys:
        rows = [
            row for row in parse_table(send_sql(
                sock,
                f"SELECT s_i_id, s_w_id, s_quantity, s_ytd, s_order_cnt FROM stock WHERE s_i_id={i_id};",
                timeout=120))
            if to_int(row["s_w_id"]) == w_id
        ]
        if not rows:
            continue
        row = rows[0]
        model["stock"][(w_id, i_id)] = {
            "s_quantity": to_int(row["s_quantity"]),
            "s_ytd": to_int(row["s_ytd"]),
            "s_order_cnt": to_int(row["s_order_cnt"]),
        }
    return model


def expect_equal(failures, label, actual, expected):
    if actual != expected:
        failures.append(f"{label}: actual={actual}, expected={expected}")


def expect_float_equal(failures, label, actual, expected):
    if actual is None or expected is None:
        if actual != expected:
            failures.append(f"{label}: actual={actual}, expected={expected}")
        return
    if abs(float(actual) - float(expected)) > 1e-4:
        failures.append(f"{label}: actual={actual}, expected={expected}")


def validate_consistency(expected_new_orders, run_probe=True):
    sock = connect(snapshot=False)
    failures = []
    model = build_snapshot_model(sock)
    for w_id in range(1, WAREHOUSES + 1):
        for d_id in range(1, DISTRICTS + 1):
            key = (w_id, d_id)
            order_ids = [o_id for o_id, _ in model["orders"][key]]
            order_ol_sum = sum(ol_cnt for _, ol_cnt in model["orders"][key])
            no_ids = model["new_orders"][key]
            raw_d_next = model["district_next"].get(key)
            raw_max_o = max(order_ids) if order_ids else None
            raw_count_no = len(no_ids)
            raw_min_no = min(no_ids) if no_ids else None
            raw_max_no = max(no_ids) if no_ids else None
            raw_count_ol = model["order_line_count"][key]

            if raw_max_o is not None and raw_d_next != raw_max_o + 1:
                failures.append(f"raw district/order mismatch ({w_id},{d_id}): d_next={raw_d_next}, max_o={raw_max_o}")
            if raw_count_no and raw_max_no is not None and raw_min_no is not None and raw_count_no != raw_max_no - raw_min_no + 1:
                failures.append(f"raw new_orders gap ({w_id},{d_id}): count={raw_count_no}, min={raw_min_no}, max={raw_max_no}")
            if raw_count_no and raw_max_o is not None and raw_max_no != raw_max_o:
                failures.append(f"raw new_orders max mismatch ({w_id},{d_id}): max_no={raw_max_no}, max_o={raw_max_o}")
            if order_ol_sum != raw_count_ol:
                failures.append(f"raw orders/order_line mismatch ({w_id},{d_id}): sum_o_ol_cnt={order_ol_sum}, count_ol={raw_count_ol}")

            d_next = query_scalar(sock, f"SELECT d_next_o_id FROM district WHERE d_w_id={w_id} AND d_id={d_id};")
            max_o = query_scalar(sock, f"SELECT MAX(o_id) as max_o_id FROM orders WHERE o_w_id={w_id} AND o_d_id={d_id};")
            max_no = query_scalar(sock, f"SELECT MAX(no_o_id) as max_no_o_id FROM new_orders WHERE no_w_id={w_id} AND no_d_id={d_id};")
            count_no = query_scalar(sock, f"SELECT COUNT(no_o_id) as count_no_o_id FROM new_orders WHERE no_w_id={w_id} AND no_d_id={d_id};")
            min_no = query_scalar(sock, f"SELECT MIN(no_o_id) as min_no_o_id FROM new_orders WHERE no_w_id={w_id} AND no_d_id={d_id};")
            sum_ol = query_scalar(sock, f"SELECT SUM(o_ol_cnt) as sum_ol_cnt FROM orders WHERE o_w_id={w_id} AND o_d_id={d_id};")
            count_ol = query_scalar(sock, f"SELECT COUNT(ol_o_id) as count_ol_o_id FROM order_line WHERE ol_w_id={w_id} AND ol_d_id={d_id};")

            expect_equal(failures, f"aggregate d_next ({w_id},{d_id})", d_next, raw_d_next)
            expect_equal(failures, f"aggregate max_o ({w_id},{d_id})", max_o, raw_max_o)
            expect_equal(failures, f"aggregate count_no ({w_id},{d_id})", count_no, raw_count_no)
            expect_equal(failures, f"aggregate min_no ({w_id},{d_id})", min_no, raw_min_no)
            expect_equal(failures, f"aggregate max_no ({w_id},{d_id})", max_no, raw_max_no)
            expect_float_equal(failures, f"aggregate sum_ol ({w_id},{d_id})", sum_ol, order_ol_sum)
            expect_equal(failures, f"aggregate count_ol ({w_id},{d_id})", count_ol, raw_count_ol)

            if d_next != max_o + 1:
                failures.append(f"district/order mismatch ({w_id},{d_id}): d_next={d_next}, max_o={max_o}")
            if count_no and max_no is not None and min_no is not None and count_no != max_no - min_no + 1:
                failures.append(f"new_orders gap ({w_id},{d_id}): count={count_no}, min={min_no}, max={max_no}")
            if count_no and max_no != max_o:
                failures.append(f"new_orders max mismatch ({w_id},{d_id}): max_no={max_no}, max_o={max_o}")
            if sum_ol != count_ol:
                failures.append(f"orders/order_line mismatch ({w_id},{d_id}): sum_o_ol_cnt={sum_ol}, count_ol={count_ol}")

    order_count = query_scalar(sock, "SELECT COUNT(*) as count_orders FROM orders;")
    expected_orders = WAREHOUSES * DISTRICTS * INITIAL_ORDERS_PER_DIST + expected_new_orders
    if order_count != expected_orders:
        failures.append(f"orders total mismatch: actual={order_count}, expected={expected_orders}")

    raw_order_count = sum(len(rows) for rows in model["orders"].values())
    expect_equal(failures, "aggregate orders total vs raw", order_count, raw_order_count)
    expect_equal(failures, "raw orders total", raw_order_count, expected_orders)

    for key, stock in model["stock"].items():
        expected_cnt = model["new_order_stock_count"][key]
        expected_ytd = model["new_order_stock_qty"][key]
        expected_quantity = 100 - expected_ytd
        if stock["s_order_cnt"] != expected_cnt:
            failures.append(f"stock order count mismatch {key}: actual={stock['s_order_cnt']}, expected={expected_cnt}")
        if stock["s_ytd"] != expected_ytd:
            failures.append(f"stock ytd mismatch {key}: actual={stock['s_ytd']}, expected={expected_ytd}")
        if stock["s_quantity"] != expected_quantity:
            failures.append(f"stock quantity mismatch {key}: actual={stock['s_quantity']}, expected={expected_quantity}")

    if run_probe:
        failures.extend(validate_index_read_path_probe(sock))
        failures.extend(validate_order_limit_probe(sock))
        failures.extend(validate_composite_index_probe(sock))
    sock.close()
    return failures


def first_data_row(resp):
    rows = parse_cells(resp)
    return rows[1] if len(rows) > 1 else None


def validate_order_limit_probe(sock):
    failures = []
    for sql in [
        "CREATE TABLE order_probe (a int, b int, c int);",
        "CREATE INDEX order_probe (a, c);",
        "INSERT INTO order_probe VALUES (1, 5, 1);",
        "INSERT INTO order_probe VALUES (1, 3, 2);",
        "INSERT INTO order_probe VALUES (2, 1, 1);",
    ]:
        resp = send_sql(sock, sql)
        if is_bad_response(resp):
            failures.append(f"order probe setup failed for `{sql}`: {resp.strip()}")
            return failures

    row = first_data_row(send_sql(
        sock, "SELECT a, b FROM order_probe ORDER BY a, b LIMIT 1;"))
    if row != ["1", "3"]:
        failures.append(f"order probe ORDER BY a,b LIMIT 1 mismatch: row={row}, expected=['1', '3']")

    row = first_data_row(send_sql(
        sock, "SELECT a, b FROM order_probe WHERE c>=0 ORDER BY a, b LIMIT 1;"))
    if row != ["1", "3"]:
        failures.append(f"order probe WHERE + ORDER BY a,b LIMIT 1 mismatch: row={row}, expected=['1', '3']")
    return failures


def validate_index_read_path_probe(sock):
    failures = []
    for sql in [
        "CREATE TABLE agg_probe (id int, k int, amount float);",
        "CREATE INDEX agg_probe (k);",
        "INSERT INTO agg_probe VALUES (1, 10, 1.0);",
        "INSERT INTO agg_probe VALUES (2, 20, 2.0);",
        "INSERT INTO agg_probe VALUES (3, 30, 3.0);",
        "UPDATE agg_probe SET k=k+100 WHERE id=1;",
        "UPDATE agg_probe SET k=k+100 WHERE id=2;",
        "DELETE FROM agg_probe WHERE id=3;",
    ]:
        resp = send_sql(sock, sql)
        if is_bad_response(resp):
            failures.append(f"probe setup failed for `{sql}`: {resp.strip()}")
            return failures

    raw_rows = table_snapshot(sock, "agg_probe")
    raw = [(to_int(r["id"]), to_int(r["k"]), to_float(r["amount"])) for r in raw_rows]
    raw_count_ge_100 = sum(1 for _, k, _ in raw if k is not None and k >= 100)
    raw_count_lt_100 = sum(1 for _, k, _ in raw if k is not None and k < 100)
    raw_sum_ge_100 = sum(amount for _, k, amount in raw if k is not None and k >= 100)

    count_ge_100 = query_scalar(sock, "SELECT COUNT(id) as cnt FROM agg_probe WHERE k>=100;")
    count_lt_100 = query_scalar(sock, "SELECT COUNT(id) as cnt FROM agg_probe WHERE k<100;")
    min_ge_100 = query_scalar(sock, "SELECT MIN(k) as min_k FROM agg_probe WHERE k>=100;")
    max_lt_100 = query_scalar(sock, "SELECT MAX(k) as max_k FROM agg_probe WHERE k<100;")
    sum_ge_100 = query_scalar(sock, "SELECT SUM(amount) as sum_amount FROM agg_probe WHERE k>=100;")

    expect_equal(failures, "probe count k>=100", count_ge_100, raw_count_ge_100)
    expect_equal(failures, "probe count k<100", count_lt_100, raw_count_lt_100)
    expect_equal(failures, "probe min k>=100", min_ge_100, min([k for _, k, _ in raw if k >= 100], default=None))
    expect_equal(failures, "probe max k<100", max_lt_100, max([k for _, k, _ in raw if k < 100], default=None))
    expect_float_equal(failures, "probe sum k>=100", sum_ge_100, raw_sum_ge_100)
    return failures


def validate_composite_index_probe(sock):
    # 用非 SI 连接建表和插数据（需要 auto-commit 生效）
    failures = []
    for sql in [
        "CREATE TABLE composite_probe (a int, b int, c int, payload int);",
        "CREATE INDEX composite_probe (a, b, c);",
        "INSERT INTO composite_probe VALUES (1, 1, 3, 30);",
        "INSERT INTO composite_probe VALUES (1, 1, 4, 40);",
        "INSERT INTO composite_probe VALUES (1, 1, 5, 50);",
        "INSERT INTO composite_probe VALUES (1, 1, 6, 60);",
        "INSERT INTO composite_probe VALUES (1, 1, 7, 70);",
        "INSERT INTO composite_probe VALUES (1, 2, 5, 125);",
        "INSERT INTO composite_probe VALUES (2, 1, 5, 215);",
        "UPDATE composite_probe SET c=8 WHERE a=1 AND b=1 AND c=4;",
        "DELETE FROM composite_probe WHERE a=1 AND b=1 AND c=7;",
    ]:
        resp = send_sql(sock, sql)
        if is_bad_response(resp):
            failures.append(f"composite probe setup failed for `{sql}`: {resp.strip()}")
            return failures

    # 关键：用 SI 连接运行查询
    # SI 下 portal.h 的 force_seq_scan 决定走 IndexScan 还是 SeqScan：
    # - abffc31 移除了复合索引的 force_seq_scan → IndexScan（有 bug → 错误结果）
    # - 安全版本仍对复合索引生效 → SeqScan（结果正确）
    # - a9e165c 修复后 IndexScan 也正确
    si_sock = connect(snapshot=True)

    # 数据：(1,1,3,30), (1,1,5,50), (1,1,6,60), (1,1,8,40), (1,2,5,125), (2,1,5,215)
    checks = [
        ("count closed-open range",
         "SELECT COUNT(payload) as cnt FROM composite_probe WHERE a=1 AND b=1 AND c>=5 AND c<8;", 2),
        ("sum closed-open range",
         "SELECT SUM(payload) as s FROM composite_probe WHERE a=1 AND b=1 AND c>=5 AND c<8;", 110),
        ("count closed range after update",
         "SELECT COUNT(payload) as cnt FROM composite_probe WHERE a=1 AND b=1 AND c>=5 AND c<=8;", 3),
        ("count deleted exact key",
         "SELECT COUNT(payload) as cnt FROM composite_probe WHERE a=1 AND b=1 AND c=7;", 0),
        ("count updated exact key",
         "SELECT COUNT(payload) as cnt FROM composite_probe WHERE a=1 AND b=1 AND c=8;", 1),
        ("prefix range excludes other b",
         "SELECT COUNT(payload) as cnt FROM composite_probe WHERE a=1 AND b=1 AND c>=0;", 4),
    ]
    for label, sql, expected in checks:
        actual = query_scalar(si_sock, sql)
        expect_float_equal(failures, f"composite probe {label}", actual, expected)

    row_checks = [
        ("select closed-open range",
         "SELECT payload FROM composite_probe WHERE a=1 AND b=1 AND c>=5 AND c<8;", [50, 60]),
        ("select closed range after update",
         "SELECT payload FROM composite_probe WHERE a=1 AND b=1 AND c>=5 AND c<=8;", [40, 50, 60]),
        ("select deleted exact key",
         "SELECT payload FROM composite_probe WHERE a=1 AND b=1 AND c=7;", []),
        ("select updated exact key",
         "SELECT payload FROM composite_probe WHERE a=1 AND b=1 AND c=8;", [40]),
        ("select prefix range excludes other b",
         "SELECT payload FROM composite_probe WHERE a=1 AND b=1 AND c>=0;", [30, 40, 50, 60]),
    ]
    for label, sql, expected in row_checks:
        rows = parse_table(send_sql(si_sock, sql))
        actual = sorted(to_int(row["payload"]) for row in rows)
        if actual != expected:
            failures.append(f"composite probe {label}: actual={actual}, expected={expected}")

    # 关键：测试同一列多个范围条件（抓 compare_first_col_key 只比较第一列的 bug）
    # 有 bug 时 lower/upper bound 不会更新到更紧的值，导致扫描范围错误
    multi_range_checks = [
        # 同列双下界：looser(c>=3)在前，tighter(c>=5)在后
        # 有 bug: lower=(1,1,3) → c=3,5,6,8 → [30,40,50,60]
        # 无 bug: lower=(1,1,5) → c=5,6,8 → [40,50,60]
        ("dual lower bound looser-first",
         "SELECT payload FROM composite_probe WHERE a=1 AND b=1 AND c>=3 AND c>=5;", [40, 50, 60]),
        ("dual lower bound tighter-first",
         "SELECT payload FROM composite_probe WHERE a=1 AND b=1 AND c>=5 AND c>=3;", [40, 50, 60]),
        # 同列双上界：looser(c<=8)在前，tighter(c<=6)在后
        # 有 bug: upper=(1,1,8) → c=3,5,6,8 → [30,40,50,60]
        # 无 bug: upper=(1,1,6) → c=3,5,6 → [30,50,60]
        ("dual upper bound looser-first",
         "SELECT payload FROM composite_probe WHERE a=1 AND b=1 AND c<=8 AND c<=6;", [30, 50, 60]),
        ("dual upper bound tighter-first",
         "SELECT payload FROM composite_probe WHERE a=1 AND b=1 AND c<=6 AND c<=8;", [30, 50, 60]),
        # 同列四条件：两个下界+两个上界
        # 有 bug: lower=(1,1,3), upper=(1,1,8) → c=3,5,6,8 → [30,40,50,60]
        # 无 bug: lower=(1,1,5), upper=(1,1,6) → c=5,6 → [50,60]
        ("quad range bounds",
         "SELECT payload FROM composite_probe WHERE a=1 AND b=1 AND c>=3 AND c>=5 AND c<=8 AND c<=6;", [50, 60]),
        ("prefix a=2 range",
         "SELECT payload FROM composite_probe WHERE a=2 AND b=1 AND c>=4 AND c<=6;", [215]),
    ]
    for label, sql, expected in multi_range_checks:
        rows = parse_table(send_sql(si_sock, sql))
        actual = sorted(to_int(row["payload"]) for row in rows)
        if actual != expected:
            failures.append(f"composite probe {label}: actual={actual}, expected={expected}")

    si_sock.close()
    return failures


def main():
    global WAREHOUSES, CUSTOMERS_PER_DIST, ITEMS, INITIAL_ORDERS_PER_DIST, INITIAL_NEW_ORDER_START

    parser = argparse.ArgumentParser()
    parser.add_argument("-w", "--workers", type=int, default=4)
    parser.add_argument("-d", "--duration", type=int, default=15)
    parser.add_argument("--warehouses", type=int, default=2)
    parser.add_argument("--customers", type=int, default=CUSTOMERS_PER_DIST)
    parser.add_argument("--items", type=int, default=ITEMS)
    parser.add_argument("--initial-orders", type=int, default=INITIAL_ORDERS_PER_DIST)
    parser.add_argument("--new-order-start", type=int, default=INITIAL_NEW_ORDER_START)
    parser.add_argument("--index-order", choices=["tpcc", "column"], default="tpcc")
    parser.add_argument("--skip-probe", action="store_true")
    args = parser.parse_args()

    WAREHOUSES = args.warehouses
    CUSTOMERS_PER_DIST = args.customers
    ITEMS = args.items
    INITIAL_ORDERS_PER_DIST = args.initial_orders
    INITIAL_NEW_ORDER_START = args.new_order_start

    server = start_server()
    stats = defaultdict(int)
    stats["new_order_by_dist"] = defaultdict(int)
    lock = threading.Lock()
    try:
        setup_database(args.index_order)
        threads = [threading.Thread(target=worker, args=(i, args.duration, stats, lock))
                   for i in range(args.workers)]
        start = time.time()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        elapsed = time.time() - start
        failures = validate_consistency(stats["new_order_total"], run_probe=not args.skip_probe)
        print(f"elapsed={elapsed:.2f}s new_order={stats['new_order']} delivery={stats['delivery']} "
              f"payment={stats['payment']} order_status={stats['order_status']} "
              f"stock_level={stats['stock_level']} abort={stats['abort']} "
              f"committed_new_orders={stats['new_order_total']}")
        if failures:
            print("CONSISTENCY CHECK FAILED")
            for failure in failures:
                print(f"  - {failure}")
            return 1
        print("CONSISTENCY CHECK PASSED")
        return 0
    finally:
        subprocess.run(["pkill", "-f", f"rmdb {DB_NAME}"], capture_output=True)
        try:
            server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
        subprocess.run(["rm", "-rf", DB_NAME], capture_output=True)


if __name__ == "__main__":
    raise SystemExit(main())
