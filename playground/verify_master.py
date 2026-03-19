#!/usr/bin/env python3
import socket
import json
import time
import os

def test_master_routing():
    host = "127.0.0.1"
    port = 9998
    
    # Create a TCP Server
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((host, port))
    server.listen(1)
    
    print(f"Test TCP Server listening on {port}...")
    
    # Start Luotsi in the background now that we are listening
    # (Actually we will run this script and let it wait for Luotsi)
    
    conn, addr = server.accept()
    print(f"Luotsi connected from {addr}")
    
    def send(msg):
        # Wrap in luotsi.forward as required by JsonRpcTcpAdapter
        wrapped = {
            "jsonrpc": "2.0",
            "method": "luotsi.forward",
            "params": {
                "source_id": "test_port",
                "target_id": "luotsi-hub",
                "payload": msg
            }
        }
        conn.sendall((json.dumps(wrapped) + "\n").encode())
        
    def recv():
        data = conn.recv(4096).decode()
        if not data: return []
        messages = []
        for line in data.split("\n"):
            if line.strip():
                msg = json.loads(line)
                # Unpack if it's a luotsi.forward
                if msg.get("method") == "luotsi.forward":
                    messages.append(msg["params"]["payload"])
                else:
                    messages.append(msg)
        return messages

    # 1. Authenticate
    print("Sending authentication request...")
    send({
        "jsonrpc": "2.0",
        "method": "luotsi/authenticate",
        "params": {"secret_key": "user_key_456"},
        "id": 1
    })
    
    # Wait for response
    time.sleep(0.5)
    responses = recv()
    print(f"Auth response: {responses}")

    # Wait for Luotsi to spawn the master node (it has dependencies)
    print("Waiting 5s for master node to spawn...")
    time.sleep(5)

    # 2. Send unmatched message
    print("Sending message with NO explicit route (should fallback to master)...")
    send({
        "jsonrpc": "2.0",
        "method": "custom.unrecognized_command",
        "params": {"query": "Where is my order?"},
        "id": 2
    })
    
    print("Message sent. Check Luotsi logs for 'Forwarding to master node'.")
    time.sleep(1)
    
    # 3. Test memory_mcp (save_fact) - This will be blocked for 'user' role
    print("Testing BLOCKED tool call (memory_mcp:save_fact)...")
    send({
        "jsonrpc": "2.0",
        "method": "tools/call",
        "params": {"name": "memory_mcp__save_fact", "arguments": {"key": "secret", "value": "123"}},
        "id": 3
    })
    time.sleep(1)
    
    # 4. Test cs_agent (reply) - This should be allowed
    print("Testing ALLOWED tool call (cs_agent:reply)...")
    send({
        "jsonrpc": "2.0",
        "method": "tools/call",
        "params": {"name": "cs_agent__reply", "arguments": {"message": "Hello world"}},
        "id": 4
    })
    
    # Wait for all responses
    print("Waiting for final responses (3 seconds)...")
    start_time = time.time()
    while time.time() - start_time < 5:
        responses = recv()
        for r in responses:
            print(f"Response: {r}")
        time.sleep(0.5)

    conn.close()
    server.close()

if __name__ == "__main__":
    test_master_routing()
