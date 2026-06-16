#!/usr/bin/env python3
"""RMDB 自动化测试脚本"""
import socket
import subprocess
import time
import sys
import os
import signal

def send_sql(sock, sql):
    """发送 SQL 并接收响应"""
    try:
        sock.sendall((sql + '\0').encode())
        resp = sock.recv(65536).decode(errors='replace')
        return resp
    except Exception as e:
        return f"Error: {e}"

def run_test(test_name, sqls, expected_output=None, expect_failure=False):
    """运行一个测试用例"""
    db_name = f"test_{test_name}_db"
    port = 8765

    # 清理
    subprocess.run(["pkill", "-f", f"rmdb {db_name}"], capture_output=True)
    time.sleep(0.3)
    subprocess.run(["rm", "-rf", db_name], capture_output=True)

    # 启动 server
    server = subprocess.Popen(
        ["./build/bin/rmdb", db_name],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(0.8)

    if server.poll() is not None:
        print(f"FAIL [{test_name}]: Server failed to start")
        return False

    try:
        # 连接
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect(("127.0.0.1", port))

        # 清空 output.txt
        output_path = os.path.join(db_name, "output.txt")
        if os.path.exists(output_path):
            open(output_path, 'w').close()

        # 发送 SQL
        for sql in sqls:
            resp = send_sql(sock, sql)
            time.sleep(0.15)

        time.sleep(0.3)

        # 读取 output.txt
        actual = ""
        if os.path.exists(output_path):
            with open(output_path) as f:
                actual = f.read().strip()

        sock.close()

        # 比较结果
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

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 run_test.py <test_name> <sql1> [sql2] ...")
        sys.exit(1)
    test_name = sys.argv[1]
    sqls = sys.argv[2:]
    run_test(test_name, sqls)
