#!/usr/bin/env python3
"""Comprehensive tests for Topic 5 - covers all edge cases"""
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
    db_name = f"test_comp_{test_name}_db"
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
                actual_lines = actual.split('\n')
                expected_lines = expected.split('\n')
                print(f"  Expected {len(expected_lines)} lines, got {len(actual_lines)} lines")
                for i, (a, e) in enumerate(zip(actual_lines, expected_lines)):
                    if a.strip() != e.strip():
                        print(f"  Line {i}: expected={repr(e)} got={repr(a)}")
                if len(actual_lines) != len(expected_lines):
                    print(f"  Extra actual: {actual_lines[len(expected_lines):]}")
                    print(f"  Extra expected: {expected_lines[len(actual_lines):]}")
                return False
        elif expect_failure:
            if "failure" in actual.lower():
                print(f"PASS [{test_name}] (got failure)")
                return True
            else:
                print(f"FAIL [{test_name}] (expected failure, got: {repr(actual)})")
                return False
        else:
            non_empty = [l for l in actual.split('\n') if l.strip()]
            print(f"RESULT [{test_name}]: {len(non_empty)} lines")
            for i, l in enumerate(non_empty): print(f"  [{i}] {l}")
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

    # ========== 1. Aggregate Functions ==========

    # 1.1 COUNT(*) on non-empty table
    total += 1
    if run_test("count_star", [
        "create table t (a int, b char(10))",
        "insert into t values (1, 'x')",
        "insert into t values (2, 'y')",
        "insert into t values (3, 'z')",
        "select COUNT(*) as c from t",
    ], expected_output="| c |\n| 3 |"):
        passed += 1

    # 1.2 COUNT(col) - counts non-null values
    total += 1
    if run_test("count_col", [
        "create table t (a int, b char(10))",
        "insert into t values (1, 'x')",
        "insert into t values (2, NULL)",
        "insert into t values (3, 'z')",
        "select COUNT(b) as c from t",
    ], expected_output="| c |\n| 2 |"):
        passed += 1

    # 1.3 COUNT(*) on empty table
    total += 1
    if run_test("count_empty", [
        "create table t (a int)",
        "select COUNT(*) as c from t",
    ], expected_output="| c |\n| 0 |"):
        passed += 1

    # 1.4 MAX on int column
    total += 1
    if run_test("max_int", [
        "create table t (a int)",
        "insert into t values (3)",
        "insert into t values (1)",
        "insert into t values (2)",
        "select MAX(a) as m from t",
    ], expected_output="| m |\n| 3 |"):
        passed += 1

    # 1.5 MIN on float column
    total += 1
    if run_test("min_float", [
        "create table t (a float)",
        "insert into t values (3.5)",
        "insert into t values (1.2)",
        "insert into t values (2.8)",
        "select MIN(a) as m from t",
    ], expected_output="| m |\n| 1.200000 |"):
        passed += 1

    # 1.6 SUM on int column
    total += 1
    if run_test("sum_int", [
        "create table t (a int)",
        "insert into t values (1)",
        "insert into t values (2)",
        "insert into t values (3)",
        "select SUM(a) as s from t",
    ], expected_output="| s |\n| 6 |"):
        passed += 1

    # 1.7 AVG on float column
    total += 1
    if run_test("avg_float", [
        "create table t (a float)",
        "insert into t values (2.0)",
        "insert into t values (4.0)",
        "select AVG(a) as a from t",
    ], expected_output="| a |\n| 3.000000 |"):
        passed += 1

    # 1.8 AVG on int column - should output float
    total += 1
    if run_test("avg_int", [
        "create table t (a int)",
        "insert into t values (1)",
        "insert into t values (2)",
        "insert into t values (3)",
        "select AVG(a) as a from t",
    ], expected_output="| a |\n| 2.000000 |"):
        passed += 1

    # 1.9 Multiple aggregate functions in one query
    total += 1
    if run_test("multi_agg", [
        "create table t (a int)",
        "insert into t values (1)",
        "insert into t values (2)",
        "insert into t values (3)",
        "select COUNT(*) as cnt, MAX(a) as mx, MIN(a) as mn, SUM(a) as sm, AVG(a) as av from t",
    ], expected_output="| cnt | mx | mn | sm | av |\n| 3 | 3 | 1 | 6 | 2.000000 |"):
        passed += 1

    # 1.10 Aggregate with WHERE clause
    total += 1
    if run_test("agg_where", [
        "create table t (a int, b int)",
        "insert into t values (1, 10)",
        "insert into t values (2, 20)",
        "insert into t values (3, 30)",
        "select SUM(a) as s from t where b > 15",
    ], expected_output="| s |\n| 5 |"):
        passed += 1

    # 1.11 MAX on empty table (should produce nothing)
    total += 1
    if run_test("max_empty", [
        "create table t (a int)",
        "select MAX(a) as m from t",
    ], expected_output=""):
        passed += 1

    # 1.12 SUM on empty table (should produce nothing)
    total += 1
    if run_test("sum_empty", [
        "create table t (a int)",
        "select SUM(a) as s from t",
    ], expected_output=""):
        passed += 1

    # 1.13 COUNT(*) with WHERE that filters all
    total += 1
    if run_test("count_where_empty", [
        "create table t (a int)",
        "insert into t values (1)",
        "select COUNT(*) as c from t where a > 100",
    ], expected_output="| c |\n| 0 |"):
        passed += 1

    # 1.14 MAX with WHERE that filters all (should produce nothing)
    total += 1
    if run_test("max_where_empty", [
        "create table t (a int)",
        "insert into t values (1)",
        "select MAX(a) as m from t where a > 100",
    ], expected_output=""):
        passed += 1

    # ========== 2. GROUP BY ==========

    # 2.1 Basic GROUP BY
    total += 1
    if run_test("group_basic", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 1)",
        "insert into t values ('a', 2)",
        "insert into t values ('b', 3)",
        "select grp, COUNT(*) as c from t group by grp",
    ], expected_output="| grp | c |\n| a | 2 |\n| b | 1 |"):
        passed += 1

    # 2.2 GROUP BY with multiple aggregate functions
    total += 1
    if run_test("group_multi_agg", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 1)",
        "insert into t values ('a', 2)",
        "insert into t values ('b', 3)",
        "insert into t values ('b', 4)",
        "select grp, MAX(val) as mx, MIN(val) as mn, SUM(val) as sm from t group by grp",
    ], expected_output="| grp | mx | mn | sm |\n| a | 2 | 1 | 3 |\n| b | 4 | 3 | 7 |"):
        passed += 1

    # 2.3 GROUP BY with WHERE
    total += 1
    if run_test("group_where", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 1)",
        "insert into t values ('a', 2)",
        "insert into t values ('b', 3)",
        "insert into t values ('b', 4)",
        "select grp, COUNT(*) as c from t where val > 1 group by grp",
    ], expected_output="| grp | c |\n| a | 1 |\n| b | 2 |"):
        passed += 1

    # 2.4 GROUP BY with HAVING
    total += 1
    if run_test("group_having", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 1)",
        "insert into t values ('a', 2)",
        "insert into t values ('b', 3)",
        "insert into t values ('c', 4)",
        "insert into t values ('c', 5)",
        "select grp, COUNT(*) as c from t group by grp having COUNT(*) > 1",
    ], expected_output="| grp | c |\n| a | 2 |\n| c | 2 |"):
        passed += 1

    # 2.5 GROUP BY with HAVING that filters all (empty result, no output)
    total += 1
    if run_test("group_having_empty", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 1)",
        "insert into t values ('b', 2)",
        "select grp, COUNT(*) as c from t group by grp having COUNT(*) > 10",
    ], expected_output=""):
        passed += 1

    # 2.6 GROUP BY with HAVING on aggregate not in SELECT
    total += 1
    if run_test("group_having_not_in_select", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 1)",
        "insert into t values ('a', 2)",
        "insert into t values ('a', 3)",
        "insert into t values ('b', 4)",
        "select grp, MAX(val) as mx from t group by grp having COUNT(*) > 2",
    ], expected_output="| grp | mx |\n| a | 3 |"):
        passed += 1

    # 2.7 GROUP BY with HAVING and AND
    total += 1
    if run_test("group_having_and", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 1)",
        "insert into t values ('a', 2)",
        "insert into t values ('a', 3)",
        "insert into t values ('b', 4)",
        "insert into t values ('b', 5)",
        "insert into t values ('b', 6)",
        "select grp, MAX(val) as mx, MIN(val) as mn from t group by grp having COUNT(*) > 1 and MIN(val) > 2",
    ], expected_output="| grp | mx | mn |\n| b | 6 | 4 |"):
        passed += 1

    # 2.8 GROUP BY with HAVING and AND (no spaces)
    total += 1
    if run_test("group_having_and_nospace", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 1)",
        "insert into t values ('a', 2)",
        "insert into t values ('a', 3)",
        "insert into t values ('b', 4)",
        "insert into t values ('b', 5)",
        "insert into t values ('b', 6)",
        "select grp, MAX(val) as mx, MIN(val) as mn from t group by grp having COUNT(*)>1andMIN(val)>2",
    ], expected_output="| grp | mx | mn |\n| b | 6 | 4 |"):
        passed += 1

    # ========== 3. ORDER BY ==========

    # 3.1 ORDER BY single column ASC
    total += 1
    if run_test("order_asc", [
        "create table t (a int)",
        "insert into t values (3)",
        "insert into t values (1)",
        "insert into t values (2)",
        "select a from t order by a",
    ], expected_output="| a |\n| 1 |\n| 2 |\n| 3 |"):
        passed += 1

    # 3.2 ORDER BY single column DESC
    total += 1
    if run_test("order_desc", [
        "create table t (a int)",
        "insert into t values (3)",
        "insert into t values (1)",
        "insert into t values (2)",
        "select a from t order by a desc",
    ], expected_output="| a |\n| 3 |\n| 2 |\n| 1 |"):
        passed += 1

    # 3.3 ORDER BY with LIMIT
    total += 1
    if run_test("order_limit", [
        "create table t (a int)",
        "insert into t values (3)",
        "insert into t values (1)",
        "insert into t values (2)",
        "select a from t order by a desc limit 2",
    ], expected_output="| a |\n| 3 |\n| 2 |"):
        passed += 1

    # 3.4 ORDER BY multiple columns
    total += 1
    if run_test("order_multi", [
        "create table t (a int, b int)",
        "insert into t values (1, 2)",
        "insert into t values (1, 1)",
        "insert into t values (2, 1)",
        "select a, b from t order by a, b",
    ], expected_output="| a | b |\n| 1 | 1 |\n| 1 | 2 |\n| 2 | 1 |"):
        passed += 1

    # 3.5 SELECT * with ORDER BY
    total += 1
    if run_test("star_order", [
        "create table t (a int, b int)",
        "insert into t values (2, 20)",
        "insert into t values (1, 10)",
        "select * from t order by a",
    ], expected_output="| a | b |\n| 1 | 10 |\n| 2 | 20 |"):
        passed += 1

    # ========== 4. Robustness ==========

    # 4.1 Non-group column in SELECT with GROUP BY
    total += 1
    if run_test("robust_non_group", [
        "create table t (a int, b int)",
        "insert into t values (1, 2)",
        "insert into t values (1, 3)",
        "select a, b from t group by a",
    ], expect_failure=True):
        passed += 1

    # 4.2 Aggregate in WHERE
    total += 1
    if run_test("robust_agg_where", [
        "create table t (a int)",
        "insert into t values (1)",
        "select MAX(a) as m from t where MAX(a) > 0 group by a",
    ], expect_failure=True):
        passed += 1

    # ========== 5. Edge Cases ==========

    # 5.1 Single row table
    total += 1
    if run_test("single_row", [
        "create table t (a int)",
        "insert into t values (42)",
        "select COUNT(*) as c, MAX(a) as m, MIN(a) as mn, SUM(a) as s, AVG(a) as a from t",
    ], expected_output="| c | m | mn | s | a |\n| 1 | 42 | 42 | 42 | 42.000000 |"):
        passed += 1

    # 5.2 All same values
    total += 1
    if run_test("same_values", [
        "create table t (a int)",
        "insert into t values (5)",
        "insert into t values (5)",
        "insert into t values (5)",
        "select COUNT(*) as c, MAX(a) as m, MIN(a) as mn from t",
    ], expected_output="| c | m | mn |\n| 3 | 5 | 5 |"):
        passed += 1

    # 5.3 Float precision
    total += 1
    if run_test("float_precision", [
        "create table t (a float)",
        "insert into t values (1.5)",
        "insert into t values (2.5)",
        "select SUM(a) as s, AVG(a) as a from t",
    ], expected_output="| s | a |\n| 4.000000 | 2.000000 |"):
        passed += 1

    # 5.4 Char column GROUP BY
    total += 1
    if run_test("char_group", [
        "create table t (name char(10), score int)",
        "insert into t values ('Alice', 90)",
        "insert into t values ('Bob', 85)",
        "insert into t values ('Alice', 95)",
        "select name, AVG(score) as avg_s from t group by name",
    ], expected_output="| name | avg_s |\n| Alice | 92.500000 |\n| Bob | 85.000000 |"):
        passed += 1

    # 5.5 Multiple queries in sequence
    total += 1
    expected_multi = "| mx |\n| 3 |\n| mn |\n| 1 |\n| cnt |\n| 3 |"
    if run_test("multi_queries", [
        "create table t (a int)",
        "insert into t values (1)",
        "insert into t values (2)",
        "insert into t values (3)",
        "select MAX(a) as mx from t",
        "select MIN(a) as mn from t",
        "select COUNT(*) as cnt from t",
    ], expected_output=expected_multi):
        passed += 1

    # 5.6 Empty result from HAVING then non-empty result
    total += 1
    expected_having_seq = "| grp | c |\n| a | c |\n| grp | c |\n| a | 2 |"
    if run_test("having_seq", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 1)",
        "insert into t values ('a', 2)",
        "insert into t values ('b', 3)",
        "select grp, COUNT(*) as c from t group by grp having COUNT(*) > 10",
        "select grp, COUNT(*) as c from t group by grp having COUNT(*) > 1",
    ]):
        passed += 1

    # 5.7 ORDER BY with GROUP BY
    total += 1
    if run_test("group_order", [
        "create table t (grp char(10), val int)",
        "insert into t values ('b', 1)",
        "insert into t values ('a', 2)",
        "insert into t values ('a', 3)",
        "select grp, COUNT(*) as c from t group by grp order by c desc",
    ], expected_output="| grp | c |\n| a | 2 |\n| b | 1 |"):
        passed += 1

    # 5.8 ORDER BY with GROUP BY and LIMIT
    total += 1
    if run_test("group_order_limit", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 1)",
        "insert into t values ('a', 2)",
        "insert into t values ('b', 3)",
        "insert into t values ('c', 4)",
        "insert into t values ('c', 5)",
        "insert into t values ('c', 6)",
        "select grp, COUNT(*) as c from t group by grp order by c desc limit 2",
    ], expected_output="| grp | c |\n| c | 3 |\n| a | 2 |"):
        passed += 1

    # 5.9 HAVING with MIN
    total += 1
    if run_test("having_min", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 10)",
        "insert into t values ('a', 20)",
        "insert into t values ('b', 5)",
        "insert into t values ('b', 15)",
        "select grp, MIN(val) as mn from t group by grp having MIN(val) > 8",
    ], expected_output="| grp | mn |\n| a | 10 |"):
        passed += 1

    # 5.10 HAVING with MAX
    total += 1
    if run_test("having_max", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 10)",
        "insert into t values ('a', 20)",
        "insert into t values ('b', 5)",
        "insert into t values ('b', 15)",
        "select grp, MAX(val) as mx from t group by grp having MAX(val) < 18",
    ], expected_output="| grp | mx |\n| b | 15 |"):
        passed += 1

    # 5.11 HAVING with SUM
    total += 1
    if run_test("having_sum", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 10)",
        "insert into t values ('a', 20)",
        "insert into t values ('b', 5)",
        "insert into t values ('b', 15)",
        "select grp, SUM(val) as s from t group by grp having SUM(val) > 25",
    ], expected_output="| grp | s |\n| a | 30 |"):
        passed += 1

    # 5.12 HAVING with AVG
    total += 1
    if run_test("having_avg", [
        "create table t (grp char(10), val int)",
        "insert into t values ('a', 10)",
        "insert into t values ('a', 20)",
        "insert into t values ('b', 5)",
        "insert into t values ('b', 15)",
        "select grp, AVG(val) as a from t group by grp having AVG(val) > 12",
    ], expected_output="| grp | a |\n| a | 15.000000 |"):
        passed += 1

    # 5.13 Exact test point 1 from problem description
    total += 1
    expected_tp1 = "| max_id |\n| 4 |\n| min_score |\n| 74.500000 |\n| avg_score |\n| 90.125000 |\n| course_num |\n| 8 |\n| row_num |\n| 8 |\n| row_num |\n| 0 |\n| sum_score |\n| 189.000000 |"
    if run_test("testpoint1", [
        "create table grade (course char(20),id int,score float)",
        "insert into grade values('DataStructure',1,95)",
        "insert into grade values('DataStructure',2,93.5)",
        "insert into grade values('DataStructure',4,87)",
        "insert into grade values('DataStructure',3,85)",
        "insert into grade values('DB',1,94)",
        "insert into grade values('DB',2,74.5)",
        "insert into grade values('DB',4,83)",
        "insert into grade values('DB',3,87)",
        "select MAX(id) as max_id from grade",
        "select MIN(score) as min_score from grade where course = 'DB'",
        "select AVG(score) as avg_score from grade where course = 'DataStructure'",
        "select COUNT(course) as course_num from grade",
        "select COUNT(*) as row_num from grade",
        "select COUNT(*) as row_num from grade where score < 60",
        "select SUM(score) as sum_score from grade where id = 1",
    ], expected_output=expected_tp1):
        passed += 1

    print(f"\n=== Results: {passed}/{total} passed ===")
    return passed == total

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
