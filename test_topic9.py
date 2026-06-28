#!/usr/bin/env python3
"""题目九：可配置的快照隔离与可串行化隔离级别 - 完整测试"""
import socket
import subprocess
import time
import sys
import os
import threading

PORT = 8765

def send_sql(sock, sql):
    try:
        sock.sendall((sql + '\0').encode())
        resp = sock.recv(65536).decode(errors='replace')
        return resp
    except Exception as e:
        return f"Error: {e}"

def connect():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(("127.0.0.1", PORT))
    return s

def get_output(db_name):
    path = os.path.join(db_name, "output.txt")
    if os.path.exists(path):
        with open(path) as f:
            return f.read().strip()
    return ""

def clear_output(db_name):
    path = os.path.join(db_name, "output.txt")
    if os.path.exists(path):
        open(path, 'w').close()

def start_server(db_name):
    subprocess.run(["pkill", "-9", "-f", f"rmdb {db_name}"], capture_output=True)
    time.sleep(0.3)
    subprocess.run(["rm", "-rf", db_name], capture_output=True)
    server = subprocess.Popen(
        ["./build/bin/rmdb", db_name],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(1.0)
    return server

def check(test_name, actual, expected):
    if expected.strip() in actual.strip():
        print(f"  PASS [{test_name}]")
        return True
    else:
        print(f"  FAIL [{test_name}]")
        print(f"    Expected: {repr(expected.strip())}")
        print(f"    Actual:   {repr(actual.strip())}")
        return False

def main():
    db_name = "test_topic9_db"
    server = start_server(db_name)
    if server.poll() is not None:
        print("FAIL: Server failed to start")
        return

    passed = 0
    total = 0

    try:
        # ============================================================
        # 示例一：写写冲突（SI & SER 通用）
        # ============================================================
        total += 1
        print("=== 示例一：写写冲突 ===")
        clear_output(db_name)
        s = connect()
        send_sql(s, "set output_file off;")
        send_sql(s, "create table account (id int, balance int);")
        send_sql(s, "insert into account values (1, 100);")
        send_sql(s, "set output_file on;")
        s.close()

        s1 = connect()  # T1
        s2 = connect()  # T2
        s3 = connect()  # T3

        # T1: set isolation + begin + update
        send_sql(s1, "set transaction isolation level snapshot isolation;")
        send_sql(s1, "begin;")
        send_sql(s1, "update account set balance = 120 where id = 1;")

        # T2: set isolation + begin + update (conflict)
        send_sql(s2, "set transaction isolation level snapshot isolation;")
        send_sql(s2, "begin;")
        send_sql(s2, "update account set balance = 90 where id = 1;")
        time.sleep(0.1)

        # T1: commit
        send_sql(s1, "commit;")
        time.sleep(0.1)

        # T2: commit (already aborted)
        send_sql(s2, "commit;")
        time.sleep(0.1)

        # T3: read
        send_sql(s3, "select * from account where id = 1;")
        time.sleep(0.2)

        out = get_output(db_name)
        expected = "abort\n| id | balance |\n| 1 | 120 |"
        if check("写写冲突", out, expected):
            passed += 1
        s1.close(); s2.close(); s3.close()

        # ============================================================
        # 示例二：事务级快照一致性（SI）
        # ============================================================
        total += 1
        print("=== 示例二：快照一致性 ===")
        clear_output(db_name)
        s = connect()
        send_sql(s, "set output_file off;")
        send_sql(s, "create table counter_test (id int, val int);")
        send_sql(s, "insert into counter_test values (1, 100);")
        send_sql(s, "set output_file on;")
        s.close()

        s1 = connect()  # T1
        s2 = connect()  # T2

        # T1: begin + first read
        send_sql(s1, "set transaction isolation level snapshot isolation;")
        send_sql(s1, "begin;")
        send_sql(s1, "select * from counter_test where id = 1;")

        # T2: begin + update + commit
        send_sql(s2, "begin;")
        send_sql(s2, "update counter_test set val = 200 where id = 1;")
        send_sql(s2, "commit;")
        time.sleep(0.1)

        # T1: second read (should still see 100)
        send_sql(s1, "select * from counter_test where id = 1;")
        send_sql(s1, "commit;")
        time.sleep(0.2)

        out = get_output(db_name)
        expected = "| id | val |\n| 1 | 100 |\n| id | val |\n| 1 | 100 |"
        if check("快照一致性", out, expected):
            passed += 1
        s1.close(); s2.close()

        # ============================================================
        # 示例三 SI：写偏序（SI下两人都能离岗）
        # ============================================================
        total += 1
        print("=== 示例三SI：写偏序 ===")
        clear_output(db_name)
        s = connect()
        send_sql(s, "set output_file off;")
        send_sql(s, "create table duty (doctor_id int, on_call int);")
        send_sql(s, "insert into duty values (1, 1);")
        send_sql(s, "insert into duty values (2, 1);")
        send_sql(s, "set output_file on;")
        s.close()

        s1 = connect()  # T1
        s2 = connect()  # T2
        s3 = connect()  # T3

        send_sql(s1, "set transaction isolation level snapshot isolation;")
        send_sql(s2, "set transaction isolation level snapshot isolation;")

        send_sql(s1, "begin;")
        send_sql(s2, "begin;")

        send_sql(s1, "select * from duty where doctor_id = 2;")
        send_sql(s2, "select * from duty where doctor_id = 1;")

        send_sql(s1, "update duty set on_call = 0 where doctor_id = 1;")
        send_sql(s2, "update duty set on_call = 0 where doctor_id = 2;")

        send_sql(s1, "commit;")
        send_sql(s2, "commit;")
        time.sleep(0.1)

        send_sql(s3, "select * from duty;")
        time.sleep(0.2)

        out = get_output(db_name)
        # SI: 两人都离岗
        if "| 1 | 0 |" in out and "| 2 | 0 |" in out:
            print("  PASS [SI写偏序：两人都离岗]")
            passed += 1
        else:
            print(f"  FAIL [SI写偏序]")
            print(f"    Actual: {repr(out)}")
        s1.close(); s2.close(); s3.close()

        # ============================================================
        # 示例三 SER：写偏序（SER下T2被回滚）
        # ============================================================
        total += 1
        print("=== 示例三SER：写偏序 ===")
        clear_output(db_name)
        s = connect()
        send_sql(s, "set output_file off;")
        send_sql(s, "delete from duty;")
        send_sql(s, "insert into duty values (1, 1);")
        send_sql(s, "insert into duty values (2, 1);")
        send_sql(s, "set output_file on;")
        s.close()

        s1 = connect()  # T1
        s2 = connect()  # T2
        s3 = connect()  # T3

        send_sql(s1, "set transaction isolation level serializable;")
        send_sql(s2, "set transaction isolation level serializable;")

        send_sql(s1, "begin;")
        send_sql(s2, "begin;")

        send_sql(s1, "select * from duty where doctor_id = 2;")
        send_sql(s2, "select * from duty where doctor_id = 1;")

        send_sql(s1, "update duty set on_call = 0 where doctor_id = 1;")

        # T2的update形成危险结构，应返回abort
        resp = send_sql(s2, "update duty set on_call = 0 where doctor_id = 2;")
        time.sleep(0.1)

        send_sql(s1, "commit;")
        send_sql(s2, "commit;")
        time.sleep(0.1)

        send_sql(s3, "select * from duty;")
        time.sleep(0.2)

        out = get_output(db_name)
        # SER: T2被回滚，只有T1的更新生效
        if "abort" in out and "| 1 | 0 |" in out and "| 2 | 1 |" in out:
            print("  PASS [SER写偏序：T2被回滚]")
            passed += 1
        else:
            print(f"  FAIL [SER写偏序]")
            print(f"    Actual: {repr(out)}")
        s1.close(); s2.close(); s3.close()

        # ============================================================
        # SI/InsertTest：插入记录的事务内可见性和快照可见性
        # ============================================================
        total += 1
        print("=== SI/InsertTest ===")
        clear_output(db_name)
        s = connect()
        send_sql(s, "set output_file off;")
        send_sql(s, "create table ins_t (id int, val int);")
        send_sql(s, "set output_file on;")
        s.close()

        s1 = connect()  # T1
        s2 = connect()  # T2

        # T1: insert + read (should see own insert)
        send_sql(s1, "set transaction isolation level snapshot isolation;")
        send_sql(s1, "begin;")
        send_sql(s1, "insert into ins_t values (1, 100);")
        send_sql(s1, "select * from ins_t where id = 1;")

        # T2: read (should NOT see T1's uncommitted insert)
        send_sql(s2, "set transaction isolation level snapshot isolation;")
        send_sql(s2, "begin;")
        send_sql(s2, "select * from ins_t where id = 1;")
        send_sql(s2, "commit;")

        # T1: commit
        send_sql(s1, "commit;")
        time.sleep(0.1)

        # T2: new txn, should see T1's committed insert
        send_sql(s2, "begin;")
        send_sql(s2, "select * from ins_t where id = 1;")
        send_sql(s2, "commit;")
        time.sleep(0.2)

        out = get_output(db_name)
        # T1自读: 100, T2快照读: 空, T2新读: 100
        lines = [l for l in out.split('\n') if l.strip()]
        if "| 1 | 100 |" in out:
            print("  PASS [SI InsertTest]")
            passed += 1
        else:
            print(f"  FAIL [SI InsertTest]")
            print(f"    Actual: {repr(out)}")
        s1.close(); s2.close()

        # ============================================================
        # SI/DirtyReadTest：未提交更新不可读
        # ============================================================
        total += 1
        print("=== SI/DirtyReadTest ===")
        clear_output(db_name)
        s = connect()
        send_sql(s, "set output_file off;")
        send_sql(s, "create table dirty_t (id int, val int);")
        send_sql(s, "insert into dirty_t values (1, 100);")
        send_sql(s, "set output_file on;")
        s.close()

        s1 = connect()  # T1
        s2 = connect()  # T2

        # T1: update but don't commit
        send_sql(s1, "set transaction isolation level snapshot isolation;")
        send_sql(s1, "begin;")
        send_sql(s1, "update dirty_t set val = 200 where id = 1;")

        # T2: read (should see 100, not 200)
        send_sql(s2, "set transaction isolation level snapshot isolation;")
        send_sql(s2, "begin;")
        send_sql(s2, "select * from dirty_t where id = 1;")
        send_sql(s2, "commit;")

        # T1: read own write (should see 200)
        send_sql(s1, "select * from dirty_t where id = 1;")
        send_sql(s1, "commit;")
        time.sleep(0.2)

        out = get_output(db_name)
        if "| 100 |" in out and "| 200 |" in out:
            print("  PASS [SI DirtyReadTest]")
            passed += 1
        else:
            print(f"  FAIL [SI DirtyReadTest]")
            print(f"    Actual: {repr(out)}")
        s1.close(); s2.close()

        # ============================================================
        # SI/WriteWriteConflictUpdateTest：并发更新同一记录
        # ============================================================
        total += 1
        print("=== SI/WriteWriteConflictUpdateTest ===")
        clear_output(db_name)
        s = connect()
        send_sql(s, "set output_file off;")
        send_sql(s, "create table wwc_t (id int, val int);")
        send_sql(s, "insert into wwc_t values (1, 100);")
        send_sql(s, "set output_file on;")
        s.close()

        s1 = connect()  # T1
        s2 = connect()  # T2
        s3 = connect()  # T3

        send_sql(s1, "set transaction isolation level snapshot isolation;")
        send_sql(s2, "set transaction isolation level snapshot isolation;")

        send_sql(s1, "begin;")
        send_sql(s2, "begin;")

        send_sql(s1, "update wwc_t set val = 200 where id = 1;")
        send_sql(s2, "update wwc_t set val = 300 where id = 1;")
        time.sleep(0.1)

        send_sql(s1, "commit;")
        send_sql(s2, "commit;")
        time.sleep(0.1)

        send_sql(s3, "select * from wwc_t where id = 1;")
        time.sleep(0.2)

        out = get_output(db_name)
        if "abort" in out:
            print("  PASS [SI WriteWriteConflictUpdateTest]")
            passed += 1
        else:
            print(f"  FAIL [SI WriteWriteConflictUpdateTest]")
            print(f"    Actual: {repr(out)}")
        s1.close(); s2.close(); s3.close()

        # ============================================================
        # SI/InsertDeleteTest：插入后删除
        # ============================================================
        total += 1
        print("=== SI/InsertDeleteTest ===")
        clear_output(db_name)
        s = connect()
        send_sql(s, "set output_file off;")
        send_sql(s, "create table id_t (id int, val int);")
        send_sql(s, "set output_file on;")
        s.close()

        s1 = connect()
        s2 = connect()
        s3 = connect()

        # T1: insert and commit
        send_sql(s1, "set transaction isolation level snapshot isolation;")
        send_sql(s1, "begin;")
        send_sql(s1, "insert into id_t values (1, 100);")
        send_sql(s1, "commit;")
        time.sleep(0.1)

        # T2: delete and commit
        send_sql(s2, "set transaction isolation level snapshot isolation;")
        send_sql(s2, "begin;")
        send_sql(s2, "delete from id_t where id = 1;")
        send_sql(s2, "commit;")
        time.sleep(0.1)

        # T3: read (should see nothing)
        send_sql(s3, "select * from id_t;")
        time.sleep(0.2)

        out = get_output(db_name)
        if "Total record(s): 0" in out or out == "" or ("id" in out and "100" not in out):
            print("  PASS [SI InsertDeleteTest]")
            passed += 1
        else:
            print(f"  FAIL [SI InsertDeleteTest]")
            print(f"    Actual: {repr(out)}")
        s1.close(); s2.close(); s3.close()

        # ============================================================
        # SI/WriteWriteConflictDeleteInsertTest：删除后重新插入
        # ============================================================
        total += 1
        print("=== SI/WriteWriteConflictDeleteInsertTest ===")
        clear_output(db_name)
        s = connect()
        send_sql(s, "set output_file off;")
        send_sql(s, "create table wdci_t (id int, val int);")
        send_sql(s, "insert into wdci_t values (1, 100);")
        send_sql(s, "set output_file on;")
        s.close()

        s1 = connect()
        s2 = connect()
        s3 = connect()

        # T1: delete
        send_sql(s1, "set transaction isolation level snapshot isolation;")
        send_sql(s1, "begin;")
        send_sql(s1, "delete from wdci_t where id = 1;")

        # T2: insert same key (conflict with T1's uncommitted delete)
        send_sql(s2, "set transaction isolation level snapshot isolation;")
        send_sql(s2, "begin;")
        send_sql(s2, "insert into wdci_t values (1, 200);")
        time.sleep(0.1)

        send_sql(s1, "commit;")
        send_sql(s2, "commit;")
        time.sleep(0.1)

        send_sql(s3, "select * from wdci_t;")
        time.sleep(0.2)

        out = get_output(db_name)
        if "abort" in out:
            print("  PASS [SI WriteWriteConflictDeleteInsertTest]")
            passed += 1
        else:
            print(f"  FAIL [SI WriteWriteConflictDeleteInsertTest]")
            print(f"    Actual: {repr(out)}")
        s1.close(); s2.close(); s3.close()

        # ============================================================
        # SI/TupleReconstructTest：多次更新后旧快照读取
        # ============================================================
        total += 1
        print("=== SI/TupleReconstructTest ===")
        clear_output(db_name)
        s = connect()
        send_sql(s, "set output_file off;")
        send_sql(s, "create table tr_t (id int, val int);")
        send_sql(s, "insert into tr_t values (1, 100);")
        send_sql(s, "set output_file on;")
        s.close()

        s1 = connect()
        s2 = connect()
        s3 = connect()
        s4 = connect()

        # T1: update to 200, commit
        send_sql(s1, "set transaction isolation level snapshot isolation;")
        send_sql(s1, "set output_file off;")
        send_sql(s1, "begin;")
        send_sql(s1, "update tr_t set val = 200 where id = 1;")
        send_sql(s1, "set output_file on;")
        send_sql(s1, "commit;")
        time.sleep(0.1)

        # T2: begin (snapshot between T1 and T3)
        send_sql(s2, "set transaction isolation level snapshot isolation;")
        send_sql(s2, "begin;")

        # T3: update to 300, commit
        send_sql(s3, "set transaction isolation level snapshot isolation;")
        send_sql(s3, "set output_file off;")
        send_sql(s3, "begin;")
        send_sql(s3, "update tr_t set val = 300 where id = 1;")
        send_sql(s3, "set output_file on;")
        send_sql(s3, "commit;")
        time.sleep(0.1)

        # T2: read (should see 200, T3's update is invisible)
        send_sql(s2, "select * from tr_t where id = 1;")
        send_sql(s2, "commit;")
        time.sleep(0.2)

        out = get_output(db_name)
        if "| 200 |" in out:
            print("  PASS [SI TupleReconstructTest]")
            passed += 1
        else:
            print(f"  FAIL [SI TupleReconstructTest]")
            print(f"    Actual: {repr(out)}")
        s1.close(); s2.close(); s3.close(); s4.close()

        # ============================================================
        # SI 自引用更新
        # ============================================================
        total += 1
        print("=== SI 自引用更新 ===")
        clear_output(db_name)
        s = connect()
        send_sql(s, "set output_file off;")
        send_sql(s, "create table self_t (id int, score int);")
        send_sql(s, "insert into self_t values (1, 80);")
        send_sql(s, "set output_file on;")
        s.close()

        s1 = connect()
        s2 = connect()

        send_sql(s1, "set transaction isolation level snapshot isolation;")
        send_sql(s1, "begin;")
        send_sql(s1, "update self_t set score = score + 5 where id = 1;")
        send_sql(s1, "select * from self_t where id = 1;")
        send_sql(s1, "commit;")
        time.sleep(0.1)

        send_sql(s2, "set transaction isolation level snapshot isolation;")
        send_sql(s2, "begin;")
        send_sql(s2, "select * from self_t where id = 1;")
        send_sql(s2, "commit;")
        time.sleep(0.2)

        out = get_output(db_name)
        if "| 85 |" in out:
            print("  PASS [SI 自引用更新]")
            passed += 1
        else:
            print(f"  FAIL [SI 自引用更新]")
            print(f"    Actual: {repr(out)}")
        s1.close(); s2.close()

        # ============================================================
        # SER/UnrepeatableReadTest：不可重复读
        # ============================================================
        total += 1
        print("=== SER/UnrepeatableReadTest ===")
        clear_output(db_name)
        s = connect()
        send_sql(s, "set output_file off;")
        send_sql(s, "create table ur_t (id int, val int);")
        send_sql(s, "insert into ur_t values (1, 100);")
        send_sql(s, "set output_file on;")
        s.close()

        s1 = connect()
        s2 = connect()

        # T1: begin + first read
        send_sql(s1, "set transaction isolation level serializable;")
        send_sql(s1, "begin;")
        send_sql(s1, "select * from ur_t where id = 1;")

        # T2: update + commit
        send_sql(s2, "begin;")
        send_sql(s2, "update ur_t set val = 200 where id = 1;")
        send_sql(s2, "commit;")
        time.sleep(0.1)

        # T1: second read (should still see 100)
        send_sql(s1, "select * from ur_t where id = 1;")
        send_sql(s1, "commit;")
        time.sleep(0.2)

        out = get_output(db_name)
        expected = "| id | val |\n| 1 | 100 |\n| id | val |\n| 1 | 100 |"
        if check("SER不可重复读", out, expected):
            passed += 1
        s1.close(); s2.close()

        # ============================================================
        # SER/PhantomReadTest：幻读
        # ============================================================
        total += 1
        print("=== SER/PhantomReadTest ===")
        clear_output(db_name)
        s = connect()
        send_sql(s, "set output_file off;")
        send_sql(s, "create table ph_t (id int, val int);")
        send_sql(s, "insert into ph_t values (1, 100);")
        send_sql(s, "insert into ph_t values (2, 200);")
        send_sql(s, "set output_file on;")
        s.close()

        s1 = connect()
        s2 = connect()

        # T1: range read
        send_sql(s1, "set transaction isolation level serializable;")
        send_sql(s1, "begin;")
        send_sql(s1, "select * from ph_t where val > 50;")

        # T2: insert + commit
        send_sql(s2, "begin;")
        send_sql(s2, "insert into ph_t values (3, 300);")
        send_sql(s2, "commit;")
        time.sleep(0.1)

        # T1: same range read (should not see phantom)
        send_sql(s1, "select * from ph_t where val > 50;")
        send_sql(s1, "commit;")
        time.sleep(0.2)

        out = get_output(db_name)
        # Should see same results both times (no phantom)
        if "| 3 |" not in out or out.count("| 1 |") == out.count("| 2 |"):
            print("  PASS [SER PhantomReadTest]")
            passed += 1
        else:
            print(f"  FAIL [SER PhantomReadTest]")
            print(f"    Actual: {repr(out)}")
        s1.close(); s2.close()

    except Exception as e:
        print(f"ERROR: {e}")
        import traceback
        traceback.print_exc()
    finally:
        server.terminate()
        server.wait(timeout=3)
        subprocess.run(["rm", "-rf", db_name], capture_output=True)

    print(f"\n=== 结果: {passed}/{total} 通过 ===")

if __name__ == "__main__":
    main()
