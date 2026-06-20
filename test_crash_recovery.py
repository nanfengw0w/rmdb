#!/usr/bin/env python3
"""RMDB Crash Recovery Test"""
import socket
import subprocess
import time
import sys
import os

def send_sql(sock, sql):
    """Send SQL and receive response"""
    try:
        sock.sendall((sql + '\0').encode())
        resp = sock.recv(65536).decode(errors='replace')
        return resp
    except Exception as e:
        return f"Error: {e}"

def connect(port=8765):
    """Connect to the server"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    sock.connect(("127.0.0.1", port))
    return sock

def test_crash_recovery():
    db_name = "test_crash_recovery_db"
    port = 8765

    # Cleanup
    subprocess.run(["pkill", "-f", f"rmdb {db_name}"], capture_output=True)
    time.sleep(0.3)
    subprocess.run(["rm", "-rf", db_name], capture_output=True)

    # Phase 1: Start server, create table, insert data, commit, then insert without commit, crash
    print("Phase 1: Setup and crash...")
    server = subprocess.Popen(
        ["./build/bin/rmdb", db_name],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(0.8)

    if server.poll() is not None:
        print("FAIL: Server failed to start")
        return False

    try:
        sock = connect(port)

        # Create table
        resp = send_sql(sock, "create table t1 (id int, num int);")
        time.sleep(0.2)

        # Insert and commit
        resp = send_sql(sock, "begin;")
        time.sleep(0.1)
        resp = send_sql(sock, "insert into t1 values(1, 100);")
        time.sleep(0.1)
        resp = send_sql(sock, "commit;")
        time.sleep(0.2)

        # Insert without commit (should be undone after recovery)
        resp = send_sql(sock, "begin;")
        time.sleep(0.1)
        resp = send_sql(sock, "insert into t1 values(2, 200);")
        time.sleep(0.2)

        sock.close()
    except Exception as e:
        print(f"FAIL: Phase 1 error: {e}")
        server.kill()
        server.wait()
        subprocess.run(["rm", "-rf", db_name], capture_output=True)
        return False

    # Kill server (simulate crash)
    server.kill()
    server.wait()
    time.sleep(0.5)

    # Phase 2: Restart and verify recovery
    print("Phase 2: Restart and verify...")
    server = subprocess.Popen(
        ["./build/bin/rmdb", db_name],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(1.0)

    if server.poll() is not None:
        print("FAIL: Server failed to restart after crash")
        subprocess.run(["rm", "-rf", db_name], capture_output=True)
        return False

    try:
        sock = connect(port)

        # Clear output file
        output_path = os.path.join(db_name, "output.txt")
        if os.path.exists(output_path):
            open(output_path, 'w').close()

        # Query data
        resp = send_sql(sock, "select * from t1;")
        time.sleep(0.3)

        # Read output
        actual = ""
        if os.path.exists(output_path):
            with open(output_path) as f:
                actual = f.read().strip()

        print(f"Query result: {actual}")

        # Check: should have only row (1, 100), not (2, 200)
        if "1" in actual and "100" in actual:
            if "2" not in actual or "200" not in actual:
                print("PASS: Committed data recovered, uncommitted data rolled back")
                sock.close()
                server.terminate()
                server.wait()
                subprocess.run(["rm", "-rf", db_name], capture_output=True)
                return True
            else:
                print("FAIL: Uncommitted data should have been rolled back")
                print(f"  Actual: {actual}")
                sock.close()
                server.terminate()
                server.wait()
                subprocess.run(["rm", "-rf", db_name], capture_output=True)
                return False
        else:
            print(f"FAIL: Expected committed data (1, 100)")
            print(f"  Actual: {actual}")
            sock.close()
            server.terminate()
            server.wait()
            subprocess.run(["rm", "-rf", db_name], capture_output=True)
            return False

    except Exception as e:
        print(f"FAIL: Phase 2 error: {e}")
        server.terminate()
        server.wait()
        subprocess.run(["rm", "-rf", db_name], capture_output=True)
        return False

def test_checkpoint_recovery():
    db_name = "test_checkpoint_recovery_db"
    port = 8765

    # Cleanup
    subprocess.run(["pkill", "-f", f"rmdb {db_name}"], capture_output=True)
    time.sleep(0.3)
    subprocess.run(["rm", "-rf", db_name], capture_output=True)

    # Phase 1: Start server, create table, insert data, checkpoint, insert more, crash
    print("Phase 1: Setup with checkpoint and crash...")
    server = subprocess.Popen(
        ["./build/bin/rmdb", db_name],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(0.8)

    if server.poll() is not None:
        print("FAIL: Server failed to start")
        return False

    try:
        sock = connect(port)

        # Create table
        resp = send_sql(sock, "create table t1 (id int, num int);")
        time.sleep(0.2)

        # Insert and commit
        resp = send_sql(sock, "begin;")
        time.sleep(0.1)
        resp = send_sql(sock, "insert into t1 values(1, 100);")
        time.sleep(0.1)
        resp = send_sql(sock, "commit;")
        time.sleep(0.2)

        # Create checkpoint
        resp = send_sql(sock, "create static_checkpoint;")
        time.sleep(0.3)
        print(f"Checkpoint response: {resp}")

        # Insert without commit after checkpoint (should be undone)
        resp = send_sql(sock, "begin;")
        time.sleep(0.1)
        resp = send_sql(sock, "insert into t1 values(2, 200);")
        time.sleep(0.2)

        sock.close()
    except Exception as e:
        print(f"FAIL: Phase 1 error: {e}")
        server.kill()
        server.wait()
        subprocess.run(["rm", "-rf", db_name], capture_output=True)
        return False

    # Kill server (simulate crash)
    server.kill()
    server.wait()
    time.sleep(0.5)

    # Phase 2: Restart and verify recovery
    print("Phase 2: Restart and verify...")
    server = subprocess.Popen(
        ["./build/bin/rmdb", db_name],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(1.0)

    if server.poll() is not None:
        print("FAIL: Server failed to restart after crash")
        subprocess.run(["rm", "-rf", db_name], capture_output=True)
        return False

    try:
        sock = connect(port)

        # Clear output file
        output_path = os.path.join(db_name, "output.txt")
        if os.path.exists(output_path):
            open(output_path, 'w').close()

        # Query data
        resp = send_sql(sock, "select * from t1;")
        time.sleep(0.3)

        # Read output
        actual = ""
        if os.path.exists(output_path):
            with open(output_path) as f:
                actual = f.read().strip()

        print(f"Query result: {actual}")

        # Check: should have only row (1, 100), not (2, 200)
        if "1" in actual and "100" in actual:
            if "2" not in actual or "200" not in actual:
                print("PASS: Checkpoint recovery works correctly")
                sock.close()
                server.terminate()
                server.wait()
                subprocess.run(["rm", "-rf", db_name], capture_output=True)
                return True
            else:
                print("FAIL: Uncommitted data should have been rolled back")
                print(f"  Actual: {actual}")
                sock.close()
                server.terminate()
                server.wait()
                subprocess.run(["rm", "-rf", db_name], capture_output=True)
                return False
        else:
            print(f"FAIL: Expected committed data (1, 100)")
            print(f"  Actual: {actual}")
            sock.close()
            server.terminate()
            server.wait()
            subprocess.run(["rm", "-rf", db_name], capture_output=True)
            return False

    except Exception as e:
        print(f"FAIL: Phase 2 error: {e}")
        server.terminate()
        server.wait()
        subprocess.run(["rm", "-rf", db_name], capture_output=True)
        return False

if __name__ == "__main__":
    results = []
    results.append(("Basic Crash Recovery", test_crash_recovery()))
    results.append(("Checkpoint Recovery", test_checkpoint_recovery()))

    print("\n=== Results ===")
    all_pass = True
    for name, passed in results:
        status = "PASS" if passed else "FAIL"
        print(f"  {status}: {name}")
        if not passed:
            all_pass = False

    sys.exit(0 if all_pass else 1)
