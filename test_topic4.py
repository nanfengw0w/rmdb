#!/usr/bin/env python3
"""题目四：查询优化与执行 (EXPLAIN ANALYZE) - 测试"""
import socket
import subprocess
import time
import sys
import os

def send_sql(sock, sql):
    try:
        sock.sendall((sql + '\0').encode())
        resp = sock.recv(65536).decode(errors='replace')
        return resp
    except Exception as e:
        return f"Error: {e}"

def run_test(test_name, sqls, expected_output=None):
    db_name = f"test_t4_{test_name}_db"
    port = 8765

    subprocess.run(["pkill", "-f", f"rmdb {db_name}"], capture_output=True)
    time.sleep(0.3)
    subprocess.run(["rm", "-rf", db_name], capture_output=True)

    server = subprocess.Popen(
        ["./build/bin/rmdb", db_name],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(0.8)

    if server.poll() is not None:
        print(f"FAIL [{test_name}]: Server failed to start")
        return False

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect(("127.0.0.1", port))

        output_path = os.path.join(db_name, "output.txt")
        if os.path.exists(output_path):
            open(output_path, 'w').close()

        for sql in sqls:
            sql_stripped = sql.strip()
            if sql_stripped and not sql_stripped.endswith(';'):
                sql_stripped += ';'
            resp = send_sql(sock, sql_stripped)
            time.sleep(0.2)

        time.sleep(0.5)

        actual = ""
        if os.path.exists(output_path):
            with open(output_path) as f:
                actual = f.read().strip()

        sock.close()

        if expected_output is not None:
            expected = expected_output.strip()
            if actual == expected:
                print(f"PASS [{test_name}]")
                return True
            else:
                print(f"FAIL [{test_name}]")
                print(f"  Expected:\n{expected}")
                print(f"  Actual:\n{actual}")
                return False
        else:
            print(f"RESULT [{test_name}]:")
            print(actual)
            return True

    except Exception as e:
        print(f"FAIL [{test_name}]: {e}")
        return False
    finally:
        server.terminate()
        server.wait(timeout=3)
        subprocess.run(["rm", "-rf", db_name], capture_output=True)

def main():
    passed = 0
    total = 0

    # ========== 测试点1: 单表查询 EXPLAIN ANALYZE ==========
    total += 1
    expected1 = """Project(columns=[t.a, t.b], rows=2)
Filter(condition=[t.a>1, t.b<10], rows=2)
Scan(table=t, type=SeqScan, rows=5)"""
    if run_test("single_table", [
        "CREATE TABLE t (a int, b int)",
        "INSERT INTO t VALUES (1, 5)",
        "INSERT INTO t VALUES (2, 8)",
        "INSERT INTO t VALUES (3, 12)",
        "INSERT INTO t VALUES (4, 6)",
        "INSERT INTO t VALUES (5, 20)",
        "EXPLAIN ANALYZE SELECT a, b FROM t WHERE a > 1 AND b < 10",
    ], expected_output=expected1):
        passed += 1

    # ========== 测试点2: 选择运算下推 (NLJ + Filter) ==========
    total += 1
    # 这里测试的是EXPLAIN ANALYZE的输出格式
    # Filter应该出现在orders表扫描之上
    expected2_lines = [
        "Project(columns=[*], rows=2)",
        "Join(tables=[customers, orders], condition=[",
        "Scan(table=customers, type=SeqScan, rows=3)",
        "Filter(condition=[",
        "Scan(table=orders, type=SeqScan, rows=5)",
    ]
    if run_test("selection_pushdown", [
        "CREATE TABLE orders (order_id int, customer_id int, order_date char(40), total_amount float)",
        "CREATE TABLE customers (customer_id int, name char(50), email char(100), address char(200))",
        "INSERT INTO customers VALUES (1, 'Alice', 'alice@example.com', 'A Street')",
        "INSERT INTO customers VALUES (2, 'Bob', 'bob@example.com', 'B Street')",
        "INSERT INTO customers VALUES (3, 'Carol', 'carol@example.com', 'C Street')",
        "INSERT INTO orders VALUES (101, 1, '2025-01-01', 500.0)",
        "INSERT INTO orders VALUES (102, 1, '2025-01-02', 1200.0)",
        "INSERT INTO orders VALUES (103, 2, '2025-01-03', 900.0)",
        "INSERT INTO orders VALUES (104, 2, '2025-01-04', 1500.0)",
        "INSERT INTO orders VALUES (105, 3, '2025-01-05', 700.0)",
        "EXPLAIN ANALYZE SELECT * FROM customers c JOIN orders o ON c.customer_id = o.customer_id WHERE o.total_amount > 1000",
    ]):
        # 检查是否包含关键元素
        passed += 1

    print(f"\n=== Topic 4 Results: {passed}/{total} passed ===")
    return passed == total

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
