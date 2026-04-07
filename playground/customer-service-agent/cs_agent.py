#!/usr/bin/env python3
"""
Customer Service Agent MCP Server for Luotsi
Exposed as an MCP server with a reply tool and tone_guide prompt.
Uses Google Gemini (gemini-2.5-flash) for generating customer-friendly replies.
"""
import sys
import json
import os
import dotenv
from langchain_google_genai import ChatGoogleGenerativeAI
from langchain_core.messages import HumanMessage, SystemMessage

TONE_GUIDE = """You are a Customer Service Tone Formatter. Your job is to take a draft message or raw information, and rewrite it into a final, polished reply for the end customer.

Your tone should be:
- Empathetic: Acknowledge the user's feelings and situation.
- Professional: Maintain a high standard of communication.
- Concise: Don't use more words than necessary.
- Solution-oriented: Always aim to help or guide the user toward a resolution.
CRITICAL RULE: DO NOT REPLY TO THE DRAFT MESSAGE. You are the final agent speaking directly to the customer. REWRITE the draft message in your own voice to be customer-friendly.

Use the provided context to ensure accuracy. If no context is provided, format the draft message alone.
Keep replies short and friendly.
NOTE: If you encounter '[VALUE_OMITTED_DUE_TO_SIZE: ... bytes]', it simply means a specific field (like a large image signature or binary data) was too large for the processing window. Ignore that specific field but use all other available data in the context to help the user. Do not report this as a failure; simply provide the information that *is* available. """

dotenv.load_dotenv()
llm = ChatGoogleGenerativeAI(
    model="gemini-2.5-flash",
    temperature=0.3,
    api_key=os.getenv("GOOGLE_API_KEY")
)


def handle_tools_list(req_id):
    return {
        "jsonrpc": "2.0",
        "id": req_id,
        "result": {
            "tools": [
                {
                    "name": "reply",
                    "description": "Rewrite a draft message into a customer-friendly, empathetic and professional final reply.",
                    "inputSchema": {
                        "type": "object",
                        "properties": {
                            "customer_message": {"type": "string", "description": "The exact original message asked by the customer."},
                            "message": {"type": "string", "description": "The draft message or raw information to convey to the customer."},
                            "context": {"type": "string", "description": "Additional context or technical data to include in the reply"}
                        },
                        "required": ["customer_message", "message"]
                    }
                }
            ]
        }
    }


def handle_prompts_list(req_id):
    return {
        "jsonrpc": "2.0",
        "id": req_id,
        "result": {
            "prompts": [
                {
                    "name": "tone_guide",
                    "description": "The system instructions for the customer service agent tone."
                }
            ]
        }
    }


def handle_prompts_get(req_id, params):
    name = params.get("name")
    if name == "tone_guide":
        return {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "description": "Tone guide for the CS agent",
                "messages": [
                    {
                        "role": "assistant",
                        "content": {"type": "text", "text": TONE_GUIDE}
                    }
                ]
            }
        }
    return error(req_id, f"Prompt not found: {name}", code=-32601)


def handle_tools_call(req_id, params):
    name = params.get("name", "")
    args = params.get("arguments", {})

    if name == "reply":
        customer_msg = args.get("customer_message", "")
        draft_message = args.get("message", "")
        context = args.get("context", "")

        context_block = f"\n\nContext:\n{context}" if context else ""
        customer_block = f"Customer asked: \"{customer_msg}\"\n\n" if customer_msg else ""
        prompt = f"{customer_block}Draft message to rewrite into final reply:\n{draft_message}{context_block}"

        try:
            response = llm.invoke([
                SystemMessage(content=TONE_GUIDE),
                HumanMessage(content=prompt)
            ])
            return ok(req_id, response.content)
        except Exception as e:
            return error(req_id, f"LLM error: {str(e)}")

    return error(req_id, f"Unknown tool: {name}", code=-32601)


def ok(req_id, text):
    return {"jsonrpc": "2.0", "id": req_id, "result": {"content": [{"type": "text", "text": text}]}}


def error(req_id, msg, code=-32602):
    return {"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": msg}}


def main():
    sys.stderr.write("[*] CS Agent started (Gemini 2.5 Flash)\n")
    sys.stderr.flush()
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
                        "capabilities": {"tools": {}, "prompts": {}},
                        "serverInfo": {"name": "cs_agent", "version": "2.0.0"}
                    }
                }
            elif method == "tools/list":
                resp = handle_tools_list(req_id)
            elif method == "prompts/list":
                resp = handle_prompts_list(req_id)
            elif method == "prompts/get":
                resp = handle_prompts_get(req_id, params)
            elif method == "tools/call":
                resp = handle_tools_call(req_id, params)
            elif method in ("notifications/initialized", "notifications/cancelled"):
                continue
            else:
                resp = error(req_id, f"Method not found: {method}", code=-32601)

            print(json.dumps(resp, sort_keys=True), flush=True)
        except Exception as e:
            print(f"Error processing line: {e}", file=sys.stderr)


if __name__ == "__main__":
    if not os.getenv("GOOGLE_API_KEY"):
        sys.stderr.write("[!] GOOGLE_API_KEY not set\n")
        sys.exit(1)
    main()
