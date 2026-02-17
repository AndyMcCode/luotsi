import sys
import json
import time

def log(msg):
    sys.stderr.write(f"[WhatsApp Node] {msg}\n")
    sys.stderr.flush()

def main():
    log("Starting WhatsApp Integration Node...")
    
    # Simulate receiving a message from "WhatsApp" and forwarding it to Core
    time.sleep(2)
    
    # JSON-RPC Notification: Incoming message
    message = {
        "jsonrpc": "2.0",
        "method": "messaging.incoming",
        "params": {
            "from": "+1234567890",
            "body": "Check stock for Product X"
        },
        "id": 1
    }
    
    log(f"Sending to Core: {json.dumps(message)}")
    print(json.dumps(message))
    sys.stdout.flush()

    # Read response from Core (if any)
    for line in sys.stdin:
        log(f"Received from Core: {line.strip()}")

if __name__ == "__main__":
    main()
