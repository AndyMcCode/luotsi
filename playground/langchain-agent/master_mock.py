#!/usr/bin/env python3
import sys
import json

def main():
    # Simple JSON-RPC loop for a Master Node
    for line in sys.stdin:
        line = line.strip()
        if not line: continue
        try:
            req = json.loads(line)
            method = req.get("method", "")
            req_id = req.get("id")
            
            # Log to file to be sure
            with open("/tmp/master_mock.log", "a") as f:
                f.write(f"Received: {method} from {req.get('source_id', 'unknown')}\n")
            
            # Log to stderr so it shows up in Luotsi's log
            print(f"[Master Mock] Received: {method} from {req.get('source_id', 'unknown')}", file=sys.stderr)
            
            if method == "initialize":
                resp = {
                    "jsonrpc": "2.0", "id": req_id,
                    "result": {
                        "protocolVersion": "2024-11-05",
                        "capabilities": {},
                        "serverInfo": {"name": "master_mock", "version": "1.0.0"}
                    }
                }
                print(json.dumps(resp, sort_keys=True), flush=True)
            elif req_id is not None:
                # Echo back for any recognized/unrecognized requests if they have an ID
                if "params" in req and "query" in req["params"]:
                    query = req["params"]["query"]
                    if "#1234" in query:
                        result = "Order #1234 is currently being processed and will ship tomorrow."
                    else:
                        result = f"Master Node received: {method}"
                else:
                    result = f"Master Node received: {method}"
                
                resp = {
                    "jsonrpc": "2.0",
                    "id": req.get("id"),
                    "result": result
                }
                print(json.dumps(resp, sort_keys=True), flush=True)
        except Exception as e:
            print(f"Error: {e}", file=sys.stderr)

if __name__ == "__main__":
    main()
