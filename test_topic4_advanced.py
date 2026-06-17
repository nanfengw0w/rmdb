#!/usr/bin/env python3
"""Advanced tests for Topic 4 - EXPLAIN ANALYZE edge cases"""
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
    db_name = f"test_t4a_{test_name}_db"
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
                actual_lines = actual.split('\n')
                expected_lines = expected.split('\n')
                print(f"  Expected {len(expected_lines)} lines, got {len(actual_lines)} lines")
                for i, (a, e) in enumerate(zip(actual_lines, expected_lines)):
                    if a != e:
                        print(f"  Line {i}:")
                        print(f"    expected: {repr(e)}")
                        print(f"    got:      {repr(a)}")
                if len(actual_lines) != len(expected_lines):
                    if len(actual_lines) > len(expected_lines):
                        for i in range(len(expected_lines), len(actual_lines)):
                            print(f"  Extra actual[{i}]: {repr(actual_lines[i])}")
                    else:
                        for i in range(len(actual_lines), len(expected_lines)):
                            print(f"  Extra expected[{i}]: {repr(expected_lines[i])}")
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

    # ========== Test 1: Condition with = operator ==========
    total += 1
    expected = "Project(columns=[t.a], rows=1)\n\tFilter(condition=[t.a=5], rows=1)\n\t\tScan(table=t, type=SeqScan, rows=3)"
    if run_test("eq_cond", [
        "CREATE TABLE t (a int)",
        "INSERT INTO t VALUES (1)",
        "INSERT INTO t VALUES (5)",
        "INSERT INTO t VALUES (10)",
        "EXPLAIN ANALYZE SELECT a FROM t WHERE a = 5",
    ], expected_output=expected):
        passed += 1

    # ========== Test 2: Condition with <> operator ==========
    total += 1
    expected = "Project(columns=[t.a], rows=2)\n\tFilter(condition=[t.a<>5], rows=2)\n\t\tScan(table=t, type=SeqScan, rows=3)"
    if run_test("neq_cond", [
        "CREATE TABLE t (a int)",
        "INSERT INTO t VALUES (1)",
        "INSERT INTO t VALUES (5)",
        "INSERT INTO t VALUES (10)",
        "EXPLAIN ANALYZE SELECT a FROM t WHERE a <> 5",
    ], expected_output=expected):
        passed += 1

    # ========== Test 3: Condition with <= operator ==========
    total += 1
    expected = "Project(columns=[t.a], rows=2)\n\tFilter(condition=[t.a<=5], rows=2)\n\t\tScan(table=t, type=SeqScan, rows=3)"
    if run_test("le_cond", [
        "CREATE TABLE t (a int)",
        "INSERT INTO t VALUES (1)",
        "INSERT INTO t VALUES (5)",
        "INSERT INTO t VALUES (10)",
        "EXPLAIN ANALYZE SELECT a FROM t WHERE a <= 5",
    ], expected_output=expected):
        passed += 1

    # ========== Test 4: Condition with >= operator ==========
    total += 1
    expected = "Project(columns=[t.a], rows=2)\n\tFilter(condition=[t.a>=5], rows=2)\n\t\tScan(table=t, type=SeqScan, rows=3)"
    if run_test("ge_cond", [
        "CREATE TABLE t (a int)",
        "INSERT INTO t VALUES (1)",
        "INSERT INTO t VALUES (5)",
        "INSERT INTO t VALUES (10)",
        "EXPLAIN ANALYZE SELECT a FROM t WHERE a >= 5",
    ], expected_output=expected):
        passed += 1

    # ========== Test 5: Condition with < operator ==========
    total += 1
    expected = "Project(columns=[t.a], rows=1)\n\tFilter(condition=[t.a<5], rows=1)\n\t\tScan(table=t, type=SeqScan, rows=3)"
    if run_test("lt_cond", [
        "CREATE TABLE t (a int)",
        "INSERT INTO t VALUES (1)",
        "INSERT INTO t VALUES (5)",
        "INSERT INTO t VALUES (10)",
        "EXPLAIN ANALYZE SELECT a FROM t WHERE a < 5",
    ], expected_output=expected):
        passed += 1

    # ========== Test 6: Conditions sorted by column name ==========
    total += 1
    # t.b<10 comes before t.c>1 (alphabetical)
    expected = "Project(columns=[t.a, t.b, t.c], rows=0)\n\tFilter(condition=[t.b<10, t.c>100], rows=0)\n\t\tScan(table=t, type=SeqScan, rows=3)"
    if run_test("cond_sort", [
        "CREATE TABLE t (a int, b int, c int)",
        "INSERT INTO t VALUES (1, 5, 0)",
        "INSERT INTO t VALUES (2, 15, 200)",
        "INSERT INTO t VALUES (3, 20, 300)",
        "EXPLAIN ANALYZE SELECT a, b, c FROM t WHERE c > 100 AND b < 10",
    ], expected_output=expected):
        passed += 1

    # ========== Test 7: Three table join ==========
    total += 1
    # Join order: a⋈(b⋈c). a=2 rows, b⋈c=1 row (b.aid=2 matches c.bid=3? no. b.aid=1 matches c.bid=1? yes, 1 match)
    # a⋈(b⋈c): a(2) × b⋈c(1) = 2 rows
    # c scan under b⋈c: b(3) × c(2) = 6
    expected = """Project(columns=[*], rows=2)
\tJoin(tables=[a, b, c], condition=[b.id=c.bid], rows=2)
\t\tJoin(tables=[a, b], condition=[a.id=b.aid], rows=3)
\t\t\tScan(table=a, type=SeqScan, rows=2)
\t\t\tScan(table=b, type=SeqScan, rows=6)
\t\tScan(table=c, type=SeqScan, rows=6)"""
    if run_test("three_join", [
        "CREATE TABLE a (id int, name char(10))",
        "CREATE TABLE b (id int, aid int, val int)",
        "CREATE TABLE c (id int, bid int, data char(10))",
        "INSERT INTO a VALUES (1, 'x')",
        "INSERT INTO a VALUES (2, 'y')",
        "INSERT INTO b VALUES (1, 1, 100)",
        "INSERT INTO b VALUES (2, 1, 200)",
        "INSERT INTO b VALUES (3, 2, 300)",
        "INSERT INTO c VALUES (1, 1, 'foo')",
        "INSERT INTO c VALUES (2, 3, 'bar')",
        "EXPLAIN ANALYZE SELECT * FROM a JOIN b ON a.id = b.aid JOIN c ON b.id = c.bid",
    ], expected_output=expected):
        passed += 1

    # ========== Test 8: Join with filter on specific table ==========
    total += 1
    # a=2 rows, b=3 rows. b scan = 3×2=6. b filter(val>150): 2 pass per scan, 2×2=4
    # join: a(1) matches b(2,val=200), a(2) matches b(3,val=300) → 2 rows
    expected = """Project(columns=[*], rows=2)
\tJoin(tables=[a, b], condition=[a.id=b.aid], rows=2)
\t\tScan(table=a, type=SeqScan, rows=2)
\t\tFilter(condition=[b.val>150], rows=4)
\t\t\tScan(table=b, type=SeqScan, rows=6)"""
    if run_test("join_filter", [
        "CREATE TABLE a (id int, name char(10))",
        "CREATE TABLE b (id int, aid int, val int)",
        "INSERT INTO a VALUES (1, 'x')",
        "INSERT INTO a VALUES (2, 'y')",
        "INSERT INTO b VALUES (1, 1, 100)",
        "INSERT INTO b VALUES (2, 1, 200)",
        "INSERT INTO b VALUES (3, 2, 300)",
        "EXPLAIN ANALYZE SELECT * FROM a JOIN b ON a.id = b.aid WHERE b.val > 150",
    ], expected_output=expected):
        passed += 1

    # ========== Test 9: Single column SELECT with table prefix ==========
    total += 1
    expected = "Project(columns=[t.name], rows=3)\n\tScan(table=t, type=SeqScan, rows=3)"
    if run_test("single_col", [
        "CREATE TABLE t (name char(10), age int)",
        "INSERT INTO t VALUES ('A', 10)",
        "INSERT INTO t VALUES ('B', 20)",
        "INSERT INTO t VALUES ('C', 30)",
        "EXPLAIN ANALYZE SELECT name FROM t",
    ], expected_output=expected):
        passed += 1

    # ========== Test 10: All columns SELECT ==========
    total += 1
    expected = "Project(columns=[t.age, t.name], rows=3)\n\tScan(table=t, type=SeqScan, rows=3)"
    if run_test("all_cols", [
        "CREATE TABLE t (name char(10), age int)",
        "INSERT INTO t VALUES ('A', 10)",
        "INSERT INTO t VALUES ('B', 20)",
        "INSERT INTO t VALUES ('C', 30)",
        "EXPLAIN ANALYZE SELECT age, name FROM t",
    ], expected_output=expected):
        passed += 1

    # ========== Test 11: Empty table ==========
    total += 1
    expected = "Project(columns=[t.a], rows=0)\n\tScan(table=t, type=SeqScan, rows=0)"
    if run_test("empty_table", [
        "CREATE TABLE t (a int)",
        "EXPLAIN ANALYZE SELECT a FROM t",
    ], expected_output=expected):
        passed += 1

    # ========== Test 12: Empty table with filter ==========
    total += 1
    expected = "Project(columns=[t.a], rows=0)\n\tFilter(condition=[t.a>5], rows=0)\n\t\tScan(table=t, type=SeqScan, rows=0)"
    if run_test("empty_table_filter", [
        "CREATE TABLE t (a int)",
        "EXPLAIN ANALYZE SELECT a FROM t WHERE a > 5",
    ], expected_output=expected):
        passed += 1

    # ========== Test 13: SELECT * from single table ==========
    total += 1
    expected = "Project(columns=[*], rows=3)\n\tScan(table=t, type=SeqScan, rows=3)"
    if run_test("star_single", [
        "CREATE TABLE t (a int, b int, c int)",
        "INSERT INTO t VALUES (1, 2, 3)",
        "INSERT INTO t VALUES (4, 5, 6)",
        "INSERT INTO t VALUES (7, 8, 9)",
        "EXPLAIN ANALYZE SELECT * FROM t",
    ], expected_output=expected):
        passed += 1

    # ========== Test 14: Join with projection on both sides ==========
    total += 1
    # a=2 rows, b=3 rows. b project/scan = 3×2=6. Join = 3 rows
    expected = """Project(columns=[a.name, b.val], rows=3)
\tJoin(tables=[a, b], condition=[a.id=b.aid], rows=3)
\t\tProject(columns=[a.id, a.name], rows=2)
\t\t\tScan(table=a, type=SeqScan, rows=2)
\t\tProject(columns=[b.aid, b.val], rows=6)
\t\t\tScan(table=b, type=SeqScan, rows=6)"""
    if run_test("join_proj_both", [
        "CREATE TABLE a (id int, name char(10))",
        "CREATE TABLE b (id int, aid int, val int)",
        "INSERT INTO a VALUES (1, 'x')",
        "INSERT INTO a VALUES (2, 'y')",
        "INSERT INTO b VALUES (1, 1, 100)",
        "INSERT INTO b VALUES (2, 1, 200)",
        "INSERT INTO b VALUES (3, 2, 300)",
        "EXPLAIN ANALYZE SELECT a.name, b.val FROM a JOIN b ON a.id = b.aid",
    ], expected_output=expected):
        passed += 1

    # ========== Test 15: Condition with column-to-column comparison ==========
    total += 1
    expected = "Project(columns=[t.a, t.b], rows=2)\n\tFilter(condition=[t.a=t.b], rows=2)\n\t\tScan(table=t, type=SeqScan, rows=4)"
    if run_test("col_col_cond", [
        "CREATE TABLE t (a int, b int)",
        "INSERT INTO t VALUES (1, 1)",
        "INSERT INTO t VALUES (2, 3)",
        "INSERT INTO t VALUES (3, 3)",
        "INSERT INTO t VALUES (4, 5)",
        "EXPLAIN ANALYZE SELECT a, b FROM t WHERE a = b",
    ], expected_output=expected):
        passed += 1

    print(f"\n=== Topic 4 Advanced Results: {passed}/{total} passed ===")
    return passed == total

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
