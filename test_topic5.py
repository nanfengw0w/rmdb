#!/usr/bin/env python3
"""题目五：聚合函数与分组统计 - 完整测试"""
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
    db_name = f"test_t5_{test_name}_db"
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
        sock.settimeout(5)
        sock.connect(("127.0.0.1", port))

        output_path = os.path.join(db_name, "output.txt")
        if os.path.exists(output_path):
            open(output_path, 'w').close()

        for sql in sqls:
            # 确保SQL以分号结尾
            sql_stripped = sql.strip()
            if sql_stripped and not sql_stripped.endswith(';'):
                sql_stripped += ';'
            resp = send_sql(sock, sql_stripped)
            time.sleep(0.15)

        time.sleep(0.3)

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

    # ========== 测试点1: 单独使用聚合函数 ==========
    total += 1
    expected1 = """| max_id |
| 4 |
| min_score |
| 74.500000 |
| course_num |
| 8 |
| row_num |
| 8 |
| sum_score |
| 189.000000 |"""
    if run_test("agg_funcs", [
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
        "select COUNT(course) as course_num from grade",
        "select COUNT(*) as row_num from grade",
        "select SUM(score) as sum_score from grade where id = 1",
    ], expected_output=expected1):
        passed += 1

    # ========== 测试点2: 聚合函数加分组统计 ==========
    total += 1
    expected2_part1 = """| id | max_score | min_score | sum_score |
| 1 | 99.000000 | 92.000000 | 286.000000 |
| 2 | 93.500000 | 88.500000 | 271.000000 |
| 3 | 94.500000 | 89.500000 | 276.500000 |"""
    if run_test("group_by_having", [
        "create table grade (course char(20),id int,score float)",
        "insert into grade values('DataStructure',1,95)",
        "insert into grade values('DataStructure',2,93.5)",
        "insert into grade values('DataStructure',3,94.5)",
        "insert into grade values('ComputerNetworks',1,99)",
        "insert into grade values('ComputerNetworks',2,88.5)",
        "insert into grade values('ComputerNetworks',3,92.5)",
        "insert into grade values('C++',1,92)",
        "insert into grade values('C++',2,89)",
        "insert into grade values('C++',3,89.5)",
        "select id,MAX(score) as max_score,MIN(score) as min_score,SUM(score) as sum_score from grade group by id",
    ], expected_output=expected2_part1):
        passed += 1

    # 测试点2 续: HAVING
    total += 1
    expected2_having1 = """| id | max_score |
| 1 | 100.000000 |"""
    if run_test("having_count", [
        "create table grade (course char(20),id int,score float)",
        "insert into grade values('DataStructure',1,95)",
        "insert into grade values('DataStructure',2,93.5)",
        "insert into grade values('DataStructure',3,94.5)",
        "insert into grade values('ComputerNetworks',1,99)",
        "insert into grade values('ComputerNetworks',2,88.5)",
        "insert into grade values('ComputerNetworks',3,92.5)",
        "insert into grade values('C++',1,92)",
        "insert into grade values('C++',2,89)",
        "insert into grade values('C++',3,89.5)",
        "insert into grade values('ParallelCompute',1,100)",
        "select id,MAX(score) as max_score from grade group by id having COUNT(*) > 3",
    ], expected_output=expected2_having1):
        passed += 1

    # 测试点2 续: HAVING with MIN
    total += 1
    expected2_having2 = """| id | max_score | min_score |
| 1 | 100.000000 | 92.000000 |
| 2 | 93.500000 | 88.500000 |
| 3 | 94.500000 | 89.500000 |"""
    if run_test("having_min", [
        "create table grade (course char(20),id int,score float)",
        "insert into grade values('DataStructure',1,95)",
        "insert into grade values('DataStructure',2,93.5)",
        "insert into grade values('DataStructure',3,94.5)",
        "insert into grade values('ComputerNetworks',1,99)",
        "insert into grade values('ComputerNetworks',2,88.5)",
        "insert into grade values('ComputerNetworks',3,92.5)",
        "insert into grade values('C++',1,92)",
        "insert into grade values('C++',2,89)",
        "insert into grade values('C++',3,89.5)",
        "insert into grade values('ParallelCompute',1,100)",
        "select id,MAX(score) as max_score,MIN(score) as min_score from grade group by id having COUNT(*) > 1 and MIN(score) > 88",
    ], expected_output=expected2_having2):
        passed += 1

    # 测试点2 续: GROUP BY course
    total += 1
    expected2_course = """| course | row_num | student_num | top_score | lowest_score |
| DataStructure | 3 | 3 | 95.000000 | 93.500000 |
| ComputerNetworks | 3 | 3 | 99.000000 | 88.500000 |
| C++ | 3 | 3 | 92.000000 | 89.000000 |
| ParallelCompute | 1 | 1 | 100.000000 | 100.000000 |"""
    if run_test("group_by_course", [
        "create table grade (course char(20),id int,score float)",
        "insert into grade values('DataStructure',1,95)",
        "insert into grade values('DataStructure',2,93.5)",
        "insert into grade values('DataStructure',3,94.5)",
        "insert into grade values('ComputerNetworks',1,99)",
        "insert into grade values('ComputerNetworks',2,88.5)",
        "insert into grade values('ComputerNetworks',3,92.5)",
        "insert into grade values('C++',1,92)",
        "insert into grade values('C++',2,89)",
        "insert into grade values('C++',3,89.5)",
        "insert into grade values('ParallelCompute',1,100)",
        "select course,COUNT(*) as row_num,COUNT(id) as student_num,MAX(score) as top_score,MIN(score) as lowest_score from grade group by course",
    ], expected_output=expected2_course):
        passed += 1

    # ========== 测试点3: 健壮性测试 ==========
    total += 1
    if run_test("robust_non_group_col", [
        "create table grade (course char(20),id int,score float)",
        "insert into grade values('DataStructure',1,95)",
        "insert into grade values('DataStructure',2,93.5)",
        "insert into grade values('DataStructure',3,94.5)",
        "insert into grade values('ComputerNetworks',1,99)",
        "insert into grade values('ComputerNetworks',2,88.5)",
        "insert into grade values('ComputerNetworks',3,92.5)",
        "select id,score from grade group by course",
    ], expect_failure=True):
        passed += 1

    total += 1
    if run_test("robust_agg_in_where", [
        "create table grade (course char(20),id int,score float)",
        "insert into grade values('DataStructure',1,95)",
        "insert into grade values('DataStructure',2,93.5)",
        "insert into grade values('DataStructure',3,94.5)",
        "insert into grade values('ComputerNetworks',1,99)",
        "insert into grade values('ComputerNetworks',2,88.5)",
        "insert into grade values('ComputerNetworks',3,92.5)",
        "select id,MAX(score) as max_score where MAX(score) > 90 from grade group by id",
    ], expect_failure=True):
        passed += 1

    # ========== 测试点4: ORDER BY + LIMIT ==========
    total += 1
    expected4 = """| vendor | invoice_number | amount |
| nancy | 1001 | 89.750000 |
| sunny | 1001 | 95.250000 |"""
    if run_test("order_by_limit", [
        "create table records (vendor char(5), invoice_number int, amount float)",
        "insert into records values('alpha', 1001, 98.0)",
        "insert into records values('bravo', 2002, 76.5)",
        "insert into records values('charl', 3003, 99.0)",
        "insert into records values('delta', 1001, 98.5)",
        "insert into records values('echoo', 4004, 88.25)",
        "insert into records values('foxxx', 4004, 77.0)",
        "insert into records values('golfy', 5005, 97.75)",
        "insert into records values('hotel', 5005, 86.75)",
        "insert into records values('indio', 6006, 76.25)",
        "insert into records values('julie', 3003, 88.0)",
        "insert into records values('karen', 5005, 89.25)",
        "insert into records values('lenny', 2002, 91.125)",
        "insert into records values('mango', 6006, 98.5)",
        "insert into records values('nancy', 1001, 89.75)",
        "insert into records values('oscar', 2002, 90.0)",
        "insert into records values('peter', 3003, 95.0)",
        "insert into records values('quack', 6006, 88.625)",
        "insert into records values('romeo', 4004, 92.0)",
        "insert into records values('sunny', 1001, 95.25)",
        "insert into records values('tonny', 7007, 98.125)",
        "insert into records values('ultra', 4004, 91.5)",
        "insert into records values('vivid', 7007, 98.3125)",
        "select * from records order by invoice_number, amount asc limit 2",
    ], expected_output=expected4):
        passed += 1

    print(f"\n=== Topic 5 Results: {passed}/{total} passed ===")
    return passed == total

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
