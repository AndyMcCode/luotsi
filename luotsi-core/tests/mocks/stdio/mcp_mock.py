import sys
import json

def log(msg):
    sys.stderr.write(f"[MockMCP] {msg}\n")
    sys.stderr.flush()

def main():
    log("Starting Mock MCP Node...")
    
    for line in sys.stdin:
        if not line.strip(): continue
        
        log(f"Received: {line.strip()}")
        try:
            req = json.loads(line)
            if req.get("method") == "chat.message":
                res = {
                    "jsonrpc": "2.0",
                    "result": {
                        "text": "Ack: " + req["params"]["content"]
                    },
                    "id": req.get("id")
                }
                print(json.dumps(res))
                sys.stdout.flush()
                log(f"Sent: {json.dumps(res)}")
        except Exception as e:
            log(f"Error: {e}")

if __name__ == "__main__":
    main()
