import sys
import json
import os
import time

# Store interactions as JSON Lines in a file
DATA_DIR = os.environ.get("SESSION_DATA_DIR", "/app/data")
os.makedirs(DATA_DIR, exist_ok=True)
LOG_FILE = os.path.join(DATA_DIR, "session_log.jsonl")

def log(msg):
    sys.stderr.write(f"[*] [SessionMemory] {msg}\n")
    sys.stderr.flush()

def handle_interaction(params):
    source = params.get("source", "unknown")
    target = params.get("target", "unknown")
    prompt = params.get("prompt", {})
    completion = params.get("completion", {})

    # Extract session_id for tenant context
    session_id = "default"
    if isinstance(prompt, dict):
        if "params" in prompt and isinstance(prompt["params"], dict):
            if "from" in prompt["params"]:
                session_id = prompt["params"]["from"]
            elif "session_id" in prompt["params"]:
                session_id = prompt["params"]["session_id"]
        elif "session_id" in prompt:
            session_id = prompt["session_id"]

    # Extract text from prompt/completion
    prompt_text = json.dumps(prompt, indent=2)
    completion_text = json.dumps(completion, indent=2)
    
    # Heuristics to find the 'core' message/query
    if isinstance(prompt, dict) and "params" in prompt:
        if "query" in prompt["params"]:
            prompt_text = prompt["params"]["query"]
        elif "message" in prompt["params"]:
            prompt_text = prompt["params"]["message"]
        elif "body" in prompt["params"]:
            prompt_text = prompt["params"]["body"]
        elif "arguments" in prompt["params"]:
            prompt_text = json.dumps(prompt["params"]["arguments"])
        
    if isinstance(completion, dict) and "result" in completion:
        res = completion["result"]
        if isinstance(res, str):
           completion_text = res
        elif isinstance(res, dict):
           if "reply" in res:
               completion_text = res["reply"]
           elif "content" in res and isinstance(res["content"], list):
               texts = [c["text"] for c in res["content"] if c.get("type") == "text"]
               completion_text = "\n".join(texts)
           elif "content" in res and isinstance(res["content"], str):
               completion_text = res["content"]
           else:
               completion_text = json.dumps(res, indent=2)

    # Write as a single JSON line
    entry = {
        "timestamp": time.time(),
        "session_id": session_id,
        "source": source,
        "target": target,
        "prompt": prompt_text,
        "response": completion_text
    }
    
    with open(LOG_FILE, "a") as f:
        f.write(json.dumps(entry, sort_keys=True) + "\n")
    
    log(f"Stored interaction from {source} ({len(prompt_text)} chars prompt)")

def main():
    log("Session Memory Node started (Background Observer - JSON Lines)")
    for line in sys.stdin:
        try:
            if not line.strip():
                continue
            req = json.loads(line)
            method = req.get("method")
            params = req.get("params", {})

            if method == "luotsi/interaction":
                handle_interaction(params)
            elif method == "initialize":
                resp = {
                    "jsonrpc": "2.0", "id": req.get("id"),
                    "result": {
                        "protocolVersion": "2024-11-05",
                        "capabilities": {"tools": {}},
                        "serverInfo": {"name": "session_memory", "version": "1.0.0"}
                    }
                }
                print(json.dumps(resp, sort_keys=True), flush=True)
            elif method == "tools/list":
                resp = {
                    "jsonrpc": "2.0", "id": req.get("id"),
                    "result": {
                        "tools": [
                            {
                                "name": "get_recent_history",
                                "description": "Fetch the most recent conversation history for a specific user session.",
                                "inputSchema": {
                                    "type": "object",
                                    "properties": {
                                        "session_id": {"type": "string", "description": "The user's session ID (e.g., phone number)"},
                                        "limit": {"type": "integer", "description": "Number of recent interactions to retrieve", "default": 5}
                                    },
                                    "required": ["session_id"]
                                }
                            }
                        ]
                    }
                }
                print(json.dumps(resp, sort_keys=True), flush=True)
            elif method == "tools/call":
                req_id = req.get("id")
                name = params.get("name", "")
                args = params.get("arguments", {})
                
                if name == "get_recent_history":
                    session_id = args.get("session_id")
                    limit = args.get("limit", 5)
                    
                    if not session_id:
                        print(json.dumps({"jsonrpc": "2.0", "id": req_id, "error": {"code": -32602, "message": "Missing session_id"}}, sort_keys=True), flush=True)
                        continue
                        
                    history = []
                    try:
                        with open(LOG_FILE, "r") as f:
                            lines = f.readlines()
                            for line in reversed(lines):
                                try:
                                    entry = json.loads(line)
                                    if entry.get("session_id") == session_id:
                                        history.insert(0, entry)
                                        if len(history) == limit:
                                            break
                                except:
                                    pass
                    except FileNotFoundError:
                        pass
                    
                    formatted_history = "RECENT HISTORY:\n"
                    if not history:
                        formatted_history += "No previous interactions found for this session."
                    else:
                        for idx, h in enumerate(history):
                            formatted_history += f"--- Turn {idx + 1} ---\nUser Prompt: {h.get('prompt')}\nAgent Reply: {h.get('response')}\n"
                    
                    print(json.dumps({"jsonrpc": "2.0", "id": req_id, "result": {"content": [{"type": "text", "text": formatted_history}]}}, sort_keys=True), flush=True)
            elif method in ("notifications/initialized", "notifications/cancelled"):
                continue
            
        except Exception as e:
            log(f"Error handling request: {e}")

if __name__ == "__main__":
    main()
