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
        f.write(json.dumps(entry) + "\n")
    
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
            
            # Background observer: no stdout responses
            
        except Exception as e:
            log(f"Error handling request: {e}")

if __name__ == "__main__":
    main()
