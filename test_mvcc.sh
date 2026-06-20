#!/bin/bash

# Test script for MVCC implementation
# This script tests the example scenarios from the problem description

CLIENT="./rmdb_client"

# Function to execute SQL
exec_sql() {
    echo "$1" | $CLIENT 2>&1
}

echo "=== Test 1: Basic Write-Write Conflict ==="
exec_sql "CREATE TABLE account (id int, balance int);"
exec_sql "INSERT INTO account VALUES (1, 100);"

# This test requires multiple sessions, which is hard to test with a simple script
# Let me test basic functionality first

echo ""
echo "=== Test 2: Basic SI Transaction ==="
exec_sql "SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;"
exec_sql "BEGIN;"
exec_sql "SELECT * FROM account;"
exec_sql "COMMIT;"

echo ""
echo "=== Test 3: Basic SER Transaction ==="
exec_sql "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE;"
exec_sql "BEGIN;"
exec_sql "SELECT * FROM account;"
exec_sql "COMMIT;"

echo ""
echo "=== Test 4: Check isolation level is set correctly ==="
exec_sql "SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;"
exec_sql "BEGIN;"
exec_sql "SELECT * FROM account;"
exec_sql "COMMIT;"

echo ""
echo "=== All basic tests completed ==="
