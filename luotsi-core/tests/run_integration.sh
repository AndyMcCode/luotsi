#!/bin/bash
set -e

# Run Luotsi with the integration config for 5 seconds
# We use paths relative to the build directory where this script might be run, 
# or we assume we are running from build/
# Let's support running from build/

BIN=${1:-"./luotsi"}
CONFIG=${2:-"../tests/integration_config.yaml"}

echo "Running Integration Test..."
timeout 5s $BIN --config $CONFIG > integration.log 2>&1 || true

# Check logs for success pattern
if grep -q "Routing Request chat_node -> mcp_node" integration.log; then
    echo "PASS: Message routed from Chat -> MCP"
else
    echo "FAIL: Route not found in logs"
    cat integration.log
    exit 1
fi

if grep -q "Routing mcp_node" integration.log; then
     # Note: The Mock MCP replies, but if there's no route back to chat_node in default config (it's one way in my config above),
     # the Core might warn "No route found".
     # Let's check that Core at least received the reply.
     echo "PASS: MCP replied"
else
     # It's okay if we don't see routing back if config doesn't have it, 
     # but we should see "Bus received from mcp_node"
     if grep -q "Bus received from mcp_node" integration.log; then
        echo "PASS: Core received reply from MCP"
     else
        echo "FAIL: Core did not receive reply from MCP"
        cat integration.log
        exit 1
     fi
fi

echo "Integration Test Successful"
rm integration.log
