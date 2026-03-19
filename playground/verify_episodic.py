import socket
import json
import time
import sys

def test_episodic():
    print("--- Starting Episodic Memory Test (Server Mode) ---")
    
    # We act as the SERVER because Luotsi's JsonRpcTcpAdapter acts as a CLIENT
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", 9998))
    server.listen(1)
    print("[DEBUG] Server listening on 9998... Waiting for Luotsi to connect.")
    
    server.settimeout(30.0)
    try:
        conn, addr = server.accept()
    except socket.timeout:
        print("[ERROR] Luotsi did not connect to the test server within 30s.")
        server.close()
        return

    print(f"[DEBUG] Connected by {addr}")
    
    def send(msg):
        conn.sendall((json.dumps(msg) + "\n").encode())

    def recv(timeout=10.0):
        try:
            conn.settimeout(timeout)
            data = conn.recv(16384).decode()
            if not data:
                return []
            # Handle multiple messages in one buffer
            messages = []
            for line in data.split("\n"):
                if line.strip():
                    try:
                        messages.append(json.loads(line))
                    except:
                        pass
            return messages
        except Exception as e:
            print(f"Recv error: {e}")
            return []

    try:
        # 1. Authenticate
        send({
            "jsonrpc": "2.0",
            "id": "auth_1",
            "method": "luotsi/authenticate",
            "params": {"secret_key": "user_key_456"}
        })
        time.sleep(1.0)
        auth_resp = recv()
        print(f"Auth Response: {auth_resp}")

        # 2. Trigger an interaction (Message -> Response)
        print("\n[Step 2] Sending interaction trigger (Where is order #1234?)...")
        send({
            "jsonrpc": "2.0",
            "id": "interact_2",
            "method": "custom/unregistered_query",
            "params": {"query": "Status of my order #1234 please?"}
        })
        
        # Capture the response from Master Node to finish the Interaction
        print("Waiting for response from Master Node...")
        responses = recv(timeout=10.0)
        for r in responses:
            print(f"Captured Response: {r.get('id')} -> {r.get('result') or r.get('error')}")
        
        print("Waiting 5s for session_memory indexing...")
        time.sleep(5.0) 
        
        # 3. Recall memory
        print("\n[Step 3] Calling memory_recall for 'order 1234'...")
        send({
            "jsonrpc": "2.0",
            "id": "recall_3",
            "method": "tools/call",
            "params": {
                "name": "session_memory__memory_recall",
                "arguments": {"query": "order 1234", "n": 1}
            }
        })
        
        time.sleep(3.0)
        responses = recv()
        found_success = False
        for resp in responses:
            if resp.get("id") == "recall_3":
                print(f"\nRecall Result: {json.dumps(resp, indent=2)}")
                result_str = str(resp.get("result", ""))
                if "#1234" in result_str and "tomorrow" in result_str:
                    print("\n[SUCCESS] Memory recall correctly returned the paired interaction!")
                    found_success = True
        
        if not found_success:
             print("\n[FAIL] No successful recall response found.")

    finally:
        conn.close()
        server.close()

if __name__ == "__main__":
    test_episodic()
