#!/bin/bash
# 自动化测试脚本：启动 server，发送 SQL，检查 output.txt
# 用法: bash test_topic.sh <test_name> <sql_commands>
# 示例: bash test_topic.sh "agg1" "create table t(a int);..."

set -e

TEST_NAME="${1:-test}"
DB_NAME="test_${TEST_NAME}_db"
PORT=8765
OUTPUT_FILE=""

# 清理旧进程
cleanup() {
    pkill -f "rmdb $DB_NAME" 2>/dev/null || true
    sleep 0.5
}
trap cleanup EXIT

# 删除旧数据库
rm -rf "$DB_NAME"

# 启动 server
./build/bin/rmdb "$DB_NAME" &
SERVER_PID=$!
sleep 1

# 检查 server 是否启动
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "FAIL: Server failed to start"
    exit 1
fi

# 发送 SQL 命令
send_sql() {
    local sql="$1"
    echo "$sql" | nc -q 1 127.0.0.1 $PORT 2>/dev/null || true
    sleep 0.3
}

# 清空 output.txt
> "$DB_NAME/output.txt"

# 执行所有传入的 SQL
shift
for sql in "$@"; do
    send_sql "$sql"
done

# 等待写入完成
sleep 0.5

# 输出结果
echo "=== Output for $TEST_NAME ==="
cat "$DB_NAME/output.txt"
echo "=== End Output ==="

# 停止 server
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true
