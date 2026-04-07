import sys
import json
import time

def main():
    for line in sys.stdin:
        line_str = line.strip()
        if not line_str: continue
        try:
            req = json.loads(line_str)
            if "method" in req:
                resp = {"jsonrpc": "2.0", "id": req.get("id")}
                if req["method"] == "initialize":
                    time.sleep(5) # Let Odoo finish first since our aggregator just takes the first valid result for initialize
                    resp["result"] = {"protocolVersion": "2024-11-05", "capabilities": {}, "serverInfo": {"name": "dummy_mcp", "version": "1.0"}}
                elif req["method"] == "tools/list":
                    resp["result"] = {"tools": [{"name": "dummy_ping", "description": "A dummy ping tool", "inputSchema": {"type": "object", "properties": {}}}]}
                elif req["method"] == "tools/call" and req.get("params", {}).get("name") == "dummy_ping":
                    resp["result"] = {"content": [{"type": "text", "text": "Pong from dummy server!"}]}
                else:
                    resp["error"] = {"code": -32601, "message": "Method not found"}
                
                print(json.dumps(resp, sort_keys=True), flush=True)
        except Exception:
            pass

if __name__ == "__main__":
    main()
