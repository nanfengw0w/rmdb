#!/usr/bin/env python3
"""题目六：UNION 集合算子 - 完整测试"""
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

def run_test(test_name, sqls, expected_output=None, expect_failure=False):
    db_name = f"test_t6_{test_name}_db"
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
        elif expect_failure:
            if "failure" in actual.lower():
                print(f"PASS [{test_name}] (got failure as expected)")
                return True
            else:
                print(f"FAIL [{test_name}] (expected failure)")
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

    # ========== 测试点1: 基本UNION ==========
    total += 1
    expected1 = """| order_id | amount | region |
| 5 | 560.000000 | Chengdu |
| 2 | 230.500000 | Shanghai |
| 6 | 199.990005 | Wuhan |
| 1 | 150.000000 | Beijing |
| 4 | 120.000000 | Shenzhen |
| 3 | 89.989998 | Guangzhou |"""
    if run_test("basic_union", [
        "CREATE TABLE orders1 (order_id INT, amount FLOAT, region CHAR(10))",
        "CREATE TABLE orders2 (order_id INT, amount FLOAT, region CHAR(10))",
        "CREATE TABLE orders3 (order_id INT, amount FLOAT, region CHAR(10))",
        "INSERT INTO orders1 VALUES (1, 150.0, 'Beijing')",
        "INSERT INTO orders1 VALUES (2, 230.5, 'Shanghai')",
        "INSERT INTO orders1 VALUES (3, 89.99, 'Guangzhou')",
        "INSERT INTO orders2 VALUES (1, 150.0, 'Beijing')",
        "INSERT INTO orders2 VALUES (4, 120.0, 'Shenzhen')",
        "INSERT INTO orders2 VALUES (5, 560.0, 'Chengdu')",
        "INSERT INTO orders3 VALUES (1, 150.0, 'Beijing')",
        "INSERT INTO orders3 VALUES (6, 199.99, 'Wuhan')",
        "SELECT * FROM (SELECT * FROM orders1 UNION SELECT * FROM orders2 UNION SELECT * FROM orders3) AS all_orders ORDER BY amount DESC",
    ], expected_output=expected1):
        passed += 1

    # ========== 测试点2: 错误校验 ==========
    # 列数不匹配
    total += 1
    if run_test("col_count_mismatch", [
        "CREATE TABLE orders1 (order_id INT, amount FLOAT, region CHAR(10))",
        "CREATE TABLE orders2 (order_id INT, amount FLOAT, region CHAR(10))",
        "INSERT INTO orders1 VALUES (1, 150.0, 'Beijing')",
        "INSERT INTO orders2 VALUES (1, 150.0, 'Beijing')",
        "SELECT * FROM (SELECT amount FROM orders1 UNION SELECT * FROM orders2) AS all_orders",
    ], expect_failure=True):
        passed += 1

    # 类型不兼容
    total += 1
    if run_test("type_incompatible", [
        "CREATE TABLE orders1 (order_id INT, amount INT, region CHAR(10))",
        "CREATE TABLE orders2 (order_id INT, amount FLOAT, region CHAR(20))",
        "INSERT INTO orders1 VALUES (1, 150, 'Beijing')",
        "INSERT INTO orders2 VALUES (1, 150.0, 'Beijing')",
        "SELECT * FROM (SELECT amount FROM orders1 UNION SELECT region FROM orders2) AS all_orders",
    ], expect_failure=True):
        passed += 1

    # ORDER BY 未知列
    total += 1
    if run_test("orderby_unknown", [
        "CREATE TABLE orders1 (order_id INT, amount FLOAT, region CHAR(10))",
        "CREATE TABLE orders2 (order_id INT, amount FLOAT, region CHAR(20))",
        "INSERT INTO orders1 VALUES (1, 150.0, 'Beijing')",
        "INSERT INTO orders2 VALUES (1, 150.0, 'Beijing')",
        "SELECT * FROM (SELECT amount FROM orders1 UNION SELECT amount FROM orders2) AS all_orders ORDER BY order_id ASC",
    ], expect_failure=True):
        passed += 1

    # 类型提升测试: INT与FLOAT提升为FLOAT, CHAR长度提升
    total += 1
    expected_type_promo = """| order_id | amount | region |
| 5 | 560.000000 | Chengdu |
| 2 | 230.500000 | Shanghai |
| 2 | 200.000000 | Shanghai |
| 6 | 199.990005 | Wuhan |
| 1 | 150.000000 | Beijing |
| 4 | 120.000000 | Shenzhen |
| 3 | 89.989998 | Guangzhou |
| 3 | 80.000000 | Guangzhou |"""
    if run_test("type_promotion", [
        "CREATE TABLE orders1 (order_id INT, amount INT, region CHAR(10))",
        "CREATE TABLE orders2 (order_id INT, amount FLOAT, region CHAR(20))",
        "INSERT INTO orders1 VALUES (1, 150, 'Beijing')",
        "INSERT INTO orders1 VALUES (2, 200, 'Shanghai')",
        "INSERT INTO orders1 VALUES (3, 80, 'Guangzhou')",
        "INSERT INTO orders2 VALUES (1, 150.0, 'Beijing')",
        "INSERT INTO orders2 VALUES (4, 120.0, 'Shenzhen')",
        "INSERT INTO orders2 VALUES (5, 560.0, 'Chengdu')",
        "INSERT INTO orders2 VALUES (6, 199.99, 'Wuhan')",
        "INSERT INTO orders2 VALUES (2, 230.5, 'Shanghai')",
        "INSERT INTO orders2 VALUES (3, 89.99, 'Guangzhou')",
        "SELECT * FROM (SELECT * FROM orders1 UNION SELECT * FROM orders2) AS all_orders ORDER BY amount DESC, order_id ASC",
    ], expected_output=expected_type_promo):
        passed += 1

    print(f"\n=== Topic 6 Results: {passed}/{total} passed ===")
    return passed == total

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
