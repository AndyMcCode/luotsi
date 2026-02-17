import sys
import json

def log(msg):
    sys.stderr.write(f"[Odoo Node] {msg}\n")
    sys.stderr.flush()

def main():
    log("Starting Odoo MCP Server...")
    
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
            
        log(f"Received: {line}")
        
        try:
            req = json.loads(line)
            if req.get("method") == "messaging.incoming":
                # Respond to the message
                response = {
                    "jsonrpc": "2.0",
                    "id": req.get("id"),
                    "result": {
                        "status": "processed",
                        "reply": "Stock is 50 units."
                    }
                }
                print(json.dumps(response))
                sys.stdout.flush()
        except json.JSONDecodeError:
            log("Failed to parse JSON")

if __name__ == "__main__":
    main()
