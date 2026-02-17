import sys
import json
import time

def log(msg):
    sys.stderr.write(f"[MockChat] {msg}\n")
    sys.stderr.flush()

def main():
    log("Starting Mock Chat Node...")
    
    # Wait a bit for the bus to settle
    time.sleep(1)
    
    # Send a message
    msg = {
        "jsonrpc": "2.0",
        "method": "chat.message",
        "params": {
            "content": "Hello World"
        },
        "id": 1
    }
    print(json.dumps(msg))
    sys.stdout.flush()
    log(f"Sent: {json.dumps(msg)}")

    # Keep alive to receive response
    for line in sys.stdin:
        log(f"Received: {line.strip()}")

if __name__ == "__main__":
    main()
