"""
wa_gateway.py  —  Luotsi WhatsApp Business Cloud API Gateway Node

Luotsi stdio node that bridges the WhatsApp Cloud API with the Luotsi Core:
  • Receives inbound test prompts via HTTP POST /message on port 8090
  • Forwards them to Luotsi Core as JSON-RPC (whatsapp.message_received)
  • When Core replies, sends the text back to the WhatsApp user via Meta's Graph API

Env vars (loaded from .env in the same directory):
  ACCESS_TOKEN     — permanent system user token
  PHONE_NUMBER_ID  — WhatsApp Business phone number ID
  BUSINESS_NUMBER  — The registered business number (from/sender in test payloads)
  TEST_RECIPIENT   — Number to deliver test messages to
"""

import os
import sys
import json
import threading
import requests
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs

# ── Load .env from the directory this file lives in ──────────────────────────
_here = os.path.dirname(os.path.abspath(__file__))
_env_path = os.path.join(_here, ".env")

def _load_dotenv(path: str):
    """Minimal .env loader — no external deps needed."""
    if not os.path.exists(path):
        return
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                continue
            key, _, val = line.partition("=")
            val = val.strip().strip("'\"")
            os.environ.setdefault(key.strip(), val)

_load_dotenv(_env_path)

ACCESS_TOKEN    = os.environ["ACCESS_TOKEN"]
PHONE_NUMBER_ID = os.environ["PHONE_NUMBER_ID"]
BUSINESS_NUMBER = os.environ["BUSINESS_NUMBER"]
VERIFY_TOKEN    = os.environ.get("VERIFY_TOKEN", "luotsi_verify_123")

WA_API_URL = f"https://graph.facebook.com/v21.0/{PHONE_NUMBER_ID}/messages"
WA_HEADERS = {
    "Authorization": f"Bearer {ACCESS_TOKEN}",
    "Content-Type":  "application/json",
}

# ── Helpers ───────────────────────────────────────────────────────────────────
_msg_id_counter   = 1
_pending_requests: dict[int, tuple[threading.Event, dict]] = {}
_lock = threading.Lock()

def log(msg: str):
    sys.stderr.write(f"[WA Gateway] {msg}\n")
    sys.stderr.flush()


def send_whatsapp_message(to: str, body: str) -> dict:
    """Send a text message via the WhatsApp Cloud API. Returns the API response dict."""
    payload = {
        "messaging_product": "whatsapp",
        "to": to,
        "type": "text",
        "text": {"body": body},
    }
    try:
        resp = requests.post(WA_API_URL, headers=WA_HEADERS, json=payload, timeout=15)
        result = resp.json()
        if resp.ok:
            msg_id = result.get("messages", [{}])[0].get("id", "?")
            log(f"✅  Sent to {to} — WA message id: {msg_id}")
        else:
            log(f"❌  Meta API error: {result}")
        return result
    except Exception as exc:
        log(f"❌  Exception calling Meta API: {exc}")
        return {"error": str(exc)}


# ── HTTP server ───────────────────────────────────────────────────────────────
class _Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):  # silence default access log
        pass

    def _send_json(self, code: int, data):
        self.end_headers()
        self.wfile.write(json.dumps(data, sort_keys=True).encode())

    def do_POST(self):
        global _msg_id_counter

        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length).decode()

        # ── Case A: Standard Luotsi local prompt (POST /message) ───────────────
        if self.path == "/message":
            try:
                payload = json.loads(raw)
            except json.JSONDecodeError:
                parsed = parse_qs(raw)
                payload = {"body": parsed.get("body", [""])[0], "from": parsed.get("from", [BUSINESS_NUMBER])[0]}
            
            body   = payload.get("body", "").strip()
            sender = payload.get("from", BUSINESS_NUMBER).strip()

        # ── Case B: Real Meta Webhook (POST /webhook) ─────────────────────────
        elif self.path == "/webhook":
            try:
                payload = json.loads(raw)
                log(f"🔔  Meta Webhook received: {json.dumps(payload)}")
                
                # Check for Status Updates (sent/delivered/read)
                val = payload.get('entry', [{}])[0].get('changes', [{}])[0].get('value', {})
                status_list = val.get('statuses')
                if status_list:
                    for s in status_list:
                        msg_id = s.get("id", "unknown")
                        status = s.get("status", "unknown")
                        log(f"📉  Status update for {msg_id}: {status}")
                        # Not forwarding to Core anymore to avoid 'No route found' warnings
                    self._send_json(200, {"status": "acknowledged"})
                    return

                # Check for Incoming Messages
                messages = val.get('messages')
                if not messages:
                    self._send_json(200, {"status": "ignored"})
                    return

                message_obj = messages[0]
                body   = message_obj.get("text", {}).get("body", "").strip()
                sender = message_obj.get("from", "").strip()
            except Exception as exc:
                log(f"❌  Error parsing Meta webhook: {exc}")
                self._send_json(400, {"error": "Invalid payload"})
                return
        
        else:
            self._send_json(404, {"error": "Not found"})
            return

        if not body:
            self._send_json(400, {"error": 'Missing "body"'})
            return

        with _lock:
            msg_id = _msg_id_counter
            _msg_id_counter += 1

        rpc_msg = {
            "jsonrpc": "2.0",
            "method":  "whatsapp.message_received",
            "params":  {"from": sender, "body": body},
            "id":      msg_id,
        }

        event      = threading.Event()
        reply_data = {"sender": sender}   # ← store sender NOW so Core handler can deliver the reply
        _pending_requests[msg_id] = (event, reply_data)

        log(f"📨  Incoming prompt from {sender}: {body!r} → Core (id={msg_id})")
        print(json.dumps(rpc_msg, sort_keys=True), flush=True)

        # Wait for Core's response (max 60 s)
        if event.wait(timeout=60):
            reply = reply_data.get("reply", {})
            self._send_json(200, reply)
        else:
            self._send_json(504, {"error": "Luotsi Core timeout"})

        _pending_requests.pop(msg_id, None)

    def do_GET(self):
        """
        1. Simple health-check endpoint (/health).
        2. Meta Webhook Verification handler (/webhook).
        """
        from urllib.parse import urlparse, parse_qs
        parsed_url = urlparse(self.path)
        path = parsed_url.path
        query = parse_qs(parsed_url.query)

        if path == "/health":
            self._send_json(200, {"status": "ok", "node": "wa_gateway"})
        
        elif path == "/webhook":
            # Meta verification challenge
            mode      = query.get("hub.mode", [None])[0]
            token     = query.get("hub.verify_token", [None])[0]
            challenge = query.get("hub.challenge", [None])[0]

            if mode == "subscribe" and token == VERIFY_TOKEN:
                log("✅  Webhook verified by Meta!")
                self.send_response(200)
                self.end_headers()
                self.wfile.write(challenge.encode())
            else:
                log(f"❌  Webhook verification failed (token mismatch: {token!r})")
                self.send_response(403)
                self.end_headers()
        
        else:
            self._send_json(404, {"error": "Not found"})


def _run_http_server(port: int = 8090):
    server = HTTPServer(("0.0.0.0", port), _Handler)
    log(f"🌐  HTTP endpoint listening on http://0.0.0.0:{port}/message")
    server.serve_forever()


# ── Luotsi Core stdin reader ──────────────────────────────────────────────────
def _handle_core_message(line: str):
    """
    Process a JSON-RPC response from Luotsi Core.

    When Core sends back a result to a pending request we:
      1. Extract the reply text
      2. Send a real WhatsApp message to the original sender
      3. Signal the waiting HTTP handler thread
    """
    try:
        msg = json.loads(line)
    except json.JSONDecodeError:
        log(f"⚠️   Non-JSON from Core: {line!r}")
        return

    msg_id = msg.get("id")

    if msg_id is not None and msg_id in _pending_requests:
        event, reply_data = _pending_requests[msg_id]
        result = msg.get("result", msg)

        # Try to extract plain text from various result shapes
        reply_text = None
        if isinstance(result, str):
            reply_text = result
        elif isinstance(result, dict):
            reply_text = (
                result.get("reply")
                or result.get("body")
                or result.get("text")
                or result.get("message")
                or result.get("content")
                or json.dumps(result)
            )

        # Deliver via WhatsApp if we have text
        if reply_text:
            # Retrieve original sender from rpc_msg params stored in reply_data
            to = reply_data.get("sender", BUSINESS_NUMBER)
            send_whatsapp_message(to=to, body=str(reply_text))

        reply_data["reply"] = result
        event.set()
    else:
        log(f"ℹ️   Unrouted Core message (id={msg_id}): {line!r}")


# ── Entry point ───────────────────────────────────────────────────────────────
def main():
    log("🚀  WhatsApp Gateway Node starting…")
    log(f"    Phone Number ID : {PHONE_NUMBER_ID}")
    log(f"    Business Number : {BUSINESS_NUMBER}")

    # Start HTTP server in daemon thread
    http_thread = threading.Thread(target=_run_http_server, daemon=True)
    http_thread.start()

    # Block on Core stdin
    for raw_line in sys.stdin:
        line = raw_line.strip()
        if line:
            log(f"←  Core: {line}")
            _handle_core_message(line)


if __name__ == "__main__":
    main()
