#!/bin/bash

# 测试脚本：验证 MVCC 实现
# 测试示例一：写写冲突

CLIENT="./rmdb_client"

# 清理输出文件
rm -f output.txt

echo "=== 测试示例一：写写冲突 ==="
echo "create table account (id int, balance int);" | $CLIENT
echo "insert into account values (1, 100);" | $CLIENT

# 模拟多会话交错执行
# 由于单客户端无法模拟多会话，我们测试基本功能
echo "set transaction isolation level snapshot isolation;" | $CLIENT
echo "begin;" | $CLIENT
echo "update account set balance = 120 where id = 1;" | $CLIENT
echo "commit;" | $CLIENT

echo "select * from account;" | $CLIENT

echo ""
echo "=== 测试示例二：快照一致性 ==="
echo "create table counter_test (id int, val int);" | $CLIENT
echo "insert into counter_test values (1, 100);" | $CLIENT

echo "set transaction isolation level snapshot isolation;" | $CLIENT
echo "begin;" | $CLIENT
echo "select * from counter_test where id = 1;" | $CLIENT
echo "select * from counter_test where id = 1;" | $CLIENT
echo "commit;" | $CLIENT

echo ""
echo "=== 测试示例三：写偏序 ==="
echo "create table duty (doctor_id int, on_call int);" | $CLIENT
echo "insert into duty values (1, 1);" | $CLIENT
echo "insert into duty values (2, 1);" | $CLIENT

echo "set transaction isolation level serializable;" | $CLIENT
echo "begin;" | $CLIENT
echo "select * from duty where doctor_id = 2;" | $CLIENT
echo "update duty set on_call = 0 where doctor_id = 1;" | $CLIENT
echo "commit;" | $CLIENT

echo "select * from duty;" | $CLIENT

echo ""
echo "=== 测试完成 ==="
echo "Output file content:"
cat output.txt
