#!/usr/bin/env python3
"""
Memory MCP Server for Luotsi
Provides persistent key-value storage and ChromaDB-backed RAG for agents.
Exposed as an MCP server with tools: save_plan, get_plan, save_fact, get_fact, memory_store, memory_recall.
"""
import sys
import json
import os

# ---- Disk-persisted Key-Value Store ----
PERSIST_DIR = os.environ.get("MEMORY_DATA_DIR", "/app/chroma_db")
os.makedirs(PERSIST_DIR, exist_ok=True)
FACTS_FILE = os.path.join(PERSIST_DIR, "facts.json")

def _load_kv_store():
    """Load facts and plans from disk."""
    if os.path.exists(FACTS_FILE):
        try:
            with open(FACTS_FILE, "r") as f:
                return json.load(f)
        except (json.JSONDecodeError, IOError):
            pass
    return {"facts": {}, "plans": {}}

def _save_kv_store(store):
    """Persist facts and plans to disk."""
    with open(FACTS_FILE, "w") as f:
        json.dump(store, f, indent=2)

kv_store = _load_kv_store()

# ---- ChromaDB Vector Store (RAG) ----
try:
    import chromadb
    chroma_client = chromadb.PersistentClient(path=PERSIST_DIR)
    collection = chroma_client.get_or_create_collection(name="session_interactions")
    sys.stderr.write(f"[*] ChromaDB initialized at {PERSIST_DIR}\n")
except Exception as e:
    sys.stderr.write(f"[*] ChromaDB init failed: {e}\n")
    collection = None


def handle_tools_list(req_id):
    return {
        "jsonrpc": "2.0",
        "id": req_id,
        "result": {
            "tools": [
                {
                    "name": "save_plan",
                    "description": "Save an agent plan for a given session ID.",
                    "inputSchema": {
                        "type": "object",
                        "properties": {
                            "session_id": {"type": "string", "description": "Unique session identifier"},
                            "plan": {"type": "string", "description": "The plan content to store"}
                        },
                        "required": ["session_id", "plan"]
                    }
                },
                {
                    "name": "get_plan",
                    "description": "Retrieve an agent plan for a given session ID.",
                    "inputSchema": {
                        "type": "object",
                        "properties": {
                            "session_id": {"type": "string", "description": "Unique session identifier"}
                        },
                        "required": ["session_id"]
                    }
                },
                {
                    "name": "save_fact",
                    "description": "Store an arbitrary key/value fact in persistent shared memory.",
                    "inputSchema": {
                        "type": "object",
                        "properties": {
                            "key": {"type": "string", "description": "Fact key"},
                            "value": {"type": "string", "description": "Fact value"}
                        },
                        "required": ["key", "value"]
                    }
                },
                {
                    "name": "get_fact",
                    "description": "Retrieve a fact by key from persistent shared memory.",
                    "inputSchema": {
                        "type": "object",
                        "properties": {
                            "key": {"type": "string", "description": "Fact key"}
                        },
                        "required": ["key"]
                    }
                },
                {
                    "name": "memory_store",
                    "description": "Store a document in long-term vector memory for future semantic retrieval. Use this to save important interactions, summaries, or knowledge.",
                    "inputSchema": {
                        "type": "object",
                        "properties": {
                            "document": {"type": "string", "description": "The text content to store in vector memory"},
                            "metadata": {"type": "object", "description": "Optional metadata tags (e.g. source, topic)", "default": {}}
                        },
                        "required": ["document"]
                    }
                },
                {
                    "name": "memory_recall",
                    "description": "Recalls relevant documents from long-term vector memory using semantic search.",
                    "inputSchema": {
                        "type": "object",
                        "properties": {
                            "query": {"type": "string", "description": "The search query (e.g., 'What did the user ask about Odoo before?')"},
                            "n": {"type": "integer", "description": "Number of results to return", "default": 3}
                        },
                        "required": ["query"]
                    }
                }
            ]
        }
    }


def handle_tools_call(req_id, params):
    global kv_store
    name = params.get("name", "")
    args = params.get("arguments", {})

    if name == "save_plan":
        session_id = args.get("session_id")
        plan = args.get("plan")
        if not session_id or plan is None:
            return error(req_id, "Missing session_id or plan")
        kv_store["plans"][session_id] = plan
        _save_kv_store(kv_store)
        return ok(req_id, f"Plan saved for session '{session_id}'.")

    elif name == "get_plan":
        session_id = args.get("session_id")
        if not session_id:
            return error(req_id, "Missing session_id")
        plan = kv_store["plans"].get(session_id)
        if plan is None:
            return ok(req_id, f"No plan found for session '{session_id}'.")
        return ok(req_id, plan)

    elif name == "save_fact":
        key = args.get("key")
        value = args.get("value")
        if not key or value is None:
            return error(req_id, "Missing key or value")
        kv_store["facts"][key] = value
        _save_kv_store(kv_store)
        return ok(req_id, f"Fact '{key}' saved.")

    elif name == "get_fact":
        key = args.get("key")
        if not key:
            return error(req_id, "Missing key")
        value = kv_store["facts"].get(key)
        if value is None:
            return ok(req_id, f"No fact found for key '{key}'.")
        return ok(req_id, value)
    
    elif name == "memory_store":
        document = args.get("document")
        metadata = args.get("metadata", {})
        if not document:
            return error(req_id, "Missing document")
        if not collection:
            return error(req_id, "ChromaDB is not available")
        
        import time
        doc_id = f"doc_{int(time.time() * 1000)}"
        metadata["stored_at"] = time.time()
        
        try:
            collection.add(
                documents=[document],
                metadatas=[metadata],
                ids=[doc_id]
            )
            sys.stderr.write(f"[*] Stored document {doc_id} ({len(document)} chars)\n")
            return ok(req_id, f"Document stored as {doc_id}.")
        except Exception as e:
            return error(req_id, f"Storage failed: {e}")
        
    elif name == "memory_recall":
        query = args.get("query")
        n = args.get("n", 3)
        if not query:
            return error(req_id, "Missing query")
        if not collection:
            return error(req_id, "ChromaDB is not available")
            
        sys.stderr.write(f"[*] Memory Recall Query: {query}\n")
        try:
            results = collection.query(
                query_texts=[query],
                n_results=n
            )
            output = []
            if results and results.get('documents') and results['documents']:
                for i in range(len(results['documents'][0])):
                    doc = results['documents'][0][i]
                    meta = results['metadatas'][0][i] if results.get('metadatas') else {}
                    output.append({
                        "document": doc,
                        "metadata": meta
                    })
            if not output:
                return ok(req_id, "No matching documents found.")
            return ok(req_id, "RECALLED DOCUMENTS:\n" + json.dumps(output, indent=2))
        except Exception as e:
            return error(req_id, f"Query failed: {e}")

    return error(req_id, f"Unknown tool: {name}", code=-32601)


def ok(req_id, text):
    return {"jsonrpc": "2.0", "id": req_id, "result": {"content": [{"type": "text", "text": text}]}}


def error(req_id, msg, code=-32602):
    return {"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": msg}}


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
            method = req.get("method", "")
            req_id = req.get("id")
            params = req.get("params", {})

            if method == "initialize":
                resp = {
                    "jsonrpc": "2.0", "id": req_id,
                    "result": {
                        "protocolVersion": "2024-11-05",
                        "capabilities": {"tools": {}},
                        "serverInfo": {"name": "memory_mcp", "version": "2.0.0"}
                    }
                }
            elif method == "tools/list":
                resp = handle_tools_list(req_id)
            elif method == "tools/call":
                resp = handle_tools_call(req_id, params)
            elif method in ("notifications/initialized", "notifications/cancelled"):
                continue  # No response needed
            else:
                resp = error(req_id, f"Method not found: {method}", code=-32601)

            print(json.dumps(resp), flush=True)
        except Exception as e:
            print(json.dumps({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": f"Parse error: {e}"}}), flush=True)


if __name__ == "__main__":
    main()
