#!/usr/bin/env python3
"""Topic 5 extended tests - edge cases"""
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
    db_name = f"test_t5e_{test_name}_db"
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
            time.sleep(0.15)

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
                # Show line-by-line diff
                actual_lines = actual.split('\n')
                expected_lines = expected.split('\n')
                print(f"  Expected {len(expected_lines)} lines, got {len(actual_lines)} lines")
                for i, (a, e) in enumerate(zip(actual_lines, expected_lines)):
                    if a != e:
                        print(f"  Line {i}: expected='{e}' got='{a}'")
                if len(actual_lines) != len(expected_lines):
                    print(f"  Extra lines in actual: {actual_lines[len(expected_lines):]}")
                    print(f"  Extra lines in expected: {expected_lines[len(actual_lines):]}")
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

    # Test 1: COUNT(*) with no matching rows
    total += 1
    expected = "| row_num |\n| 0 |"
    if run_test("count_empty", [
        "create table t (a int, b int)",
        "insert into t values (1, 2)",
        "select COUNT(*) as row_num from t where a > 100",
    ], expected_output=expected):
        passed += 1

    # Test 2: Multiple aggregate functions
    total += 1
    expected = "| cnt | max_a | min_b | sum_a | avg_b |\n| 3 | 3 | 2 | 6 | 3.000000 |"
    if run_test("multi_agg", [
        "create table t (a int, b int)",
        "insert into t values (1, 2)",
        "insert into t values (2, 3)",
        "insert into t values (3, 4)",
        "select COUNT(*) as cnt, MAX(a) as max_a, MIN(b) as min_b, SUM(a) as sum_a, AVG(b) as avg_b from t",
    ], expected_output=expected):
        passed += 1

    # Test 3: GROUP BY with ORDER BY
    total += 1
    expected = "| grp | cnt |\n| c | 3 |\n| a | 2 |\n| b | 1 |"
    if run_test("group_order", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 1)",
        "insert into t values ('b', 2)",
        "insert into t values ('c', 3)",
        "insert into t values ('a', 4)",
        "insert into t values ('c', 5)",
        "insert into t values ('c', 6)",
        "select grp, COUNT(*) as cnt from t group by grp order by cnt desc",
    ], expected_output=expected):
        passed += 1

    # Test 4: HAVING with multiple conditions (AND)
    total += 1
    expected = "| id | max_s |\n| 1 | 100.000000 |\n| 2 | 93.500000 |\n| 3 | 94.500000 |"
    if run_test("having_multi", [
        "create table g (course char(20), id int, score float)",
        "insert into g values ('DS', 1, 95)",
        "insert into g values ('DS', 2, 93.5)",
        "insert into g values ('DS', 3, 94.5)",
        "insert into g values ('CN', 1, 99)",
        "insert into g values ('CN', 2, 88.5)",
        "insert into g values ('CN', 3, 92.5)",
        "insert into g values ('C++', 1, 92)",
        "insert into g values ('C++', 2, 89)",
        "insert into g values ('C++', 3, 89.5)",
        "insert into g values ('PC', 1, 100)",
        "select id, MAX(score) as max_s from g group by id having COUNT(*) > 1 and MIN(score) > 88",
    ], expected_output=expected):
        passed += 1

    # Test 5: ORDER BY with LIMIT
    total += 1
    expected = "| name | score |\n| Carol | 95.000000 |\n| Bob | 90.000000 |"
    if run_test("order_limit", [
        "create table s (name char(10), score float)",
        "insert into s values ('Alice', 85.0)",
        "insert into s values ('Bob', 90.0)",
        "insert into s values ('Carol', 95.0)",
        "insert into s values ('Dave', 80.0)",
        "select name, score from s order by score desc limit 2",
    ], expected_output=expected):
        passed += 1

    # Test 6: select * with ORDER BY (single col)
    total += 1
    expected = "| name | score |\n| Carol | 95.000000 |\n| Bob | 90.000000 |\n| Alice | 85.000000 |\n| Dave | 80.000000 |"
    if run_test("select_star_order", [
        "create table s (name char(10), score float)",
        "insert into s values ('Alice', 85.0)",
        "insert into s values ('Bob', 90.0)",
        "insert into s values ('Carol', 95.0)",
        "insert into s values ('Dave', 80.0)",
        "select * from s order by score desc",
    ], expected_output=expected):
        passed += 1

    # Test 7: HAVING referencing column not in SELECT
    total += 1
    expected = "| id | max_s |\n| 1 | 100.000000 |"
    if run_test("having_not_in_select", [
        "create table g (course char(20), id int, score float)",
        "insert into g values ('DS', 1, 95)",
        "insert into g values ('DS', 2, 93.5)",
        "insert into g values ('DS', 3, 94.5)",
        "insert into g values ('CN', 1, 99)",
        "insert into g values ('CN', 2, 88.5)",
        "insert into g values ('CN', 3, 92.5)",
        "insert into g values ('C++', 1, 92)",
        "insert into g values ('C++', 2, 89)",
        "insert into g values ('C++', 3, 89.5)",
        "insert into g values ('PC', 1, 100)",
        "select id, MAX(score) as max_s from g group by id having COUNT(*) > 3",
    ], expected_output=expected):
        passed += 1

    # Test 8: Robustness - non-group column in SELECT
    total += 1
    if run_test("robust_non_group", [
        "create table t (a int, b int)",
        "insert into t values (1, 2)",
        "insert into t values (1, 3)",
        "insert into t values (2, 4)",
        "select a, b from t group by a",
    ], expect_failure=True):
        passed += 1

    # Test 9: Robustness - aggregate in WHERE
    total += 1
    if run_test("robust_agg_where", [
        "create table t (a int, b int)",
        "insert into t values (1, 2)",
        "select a, MAX(b) from t where MAX(b) > 1 group by a",
    ], expect_failure=True):
        passed += 1

    # Test 10: GROUP BY with char column
    total += 1
    expected = "| course | cnt |\n| DS | 3 |\n| CN | 2 |"
    if run_test("group_char", [
        "create table g (course char(20), score float)",
        "insert into g values ('DS', 90)",
        "insert into g values ('CN', 85)",
        "insert into g values ('DS', 95)",
        "insert into g values ('CN', 88)",
        "insert into g values ('DS', 92)",
        "select course, COUNT(*) as cnt from g group by course",
    ], expected_output=expected):
        passed += 1

    print(f"\n=== Extended Topic 5 Results: {passed}/{total} passed ===")
    return passed == total

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
