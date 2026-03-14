#!/bin/bash
set -e

# Run Luotsi with the JSON-RPC config
# We assume we are running from build/

BIN=${1:-"./luotsi"}
CONFIG=${2:-"../tests/jsonrpc_config.yaml"}
MOCK="tests/mocks/json_rpc2/tcp_mock.py"

echo "Starting TCP Mock Server..."
python3 $MOCK > tcp_mock.log 2>&1 &
MOCK_PID=$!

# Kill mock on exit
trap "kill $MOCK_PID" EXIT

# Wait for mock to start
sleep 1

echo "Running JSON-RPC Integration Test..."
timeout 5s $BIN --config $CONFIG > jsonrpc_test.log 2>&1 || true

# Check logs for success pattern
# 1. Did we connect?
if grep -q "connected to 127.0.0.1:8888" jsonrpc_test.log; then
    echo "PASS: Connected to TCP Mock"
else
    echo "FAIL: Connection failed"
    cat jsonrpc_test.log
    exit 1
fi

# 2. Did we send data?
if grep -q "Routing Request internal_chat -> external_rpc" jsonrpc_test.log; then
    echo "PASS: Message routed to JSON-RPC adapter"
else
    echo "FAIL: Routing to adapter failed"
    cat jsonrpc_test.log
    exit 1
fi

# 3. Did we receive the echo?
if grep -q "Bus received from external_rpc" jsonrpc_test.log; then
    echo "PASS: Received reply from JSON-RPC mock"
else
    echo "FAIL: Did not receive reply"
    cat jsonrpc_test.log
    exit 1
fi

echo "JSON-RPC Integration Test Successful"
rm jsonrpc_test.log tcp_mock.log
