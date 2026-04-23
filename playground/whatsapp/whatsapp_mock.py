import sys
import os
import json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse

# Global message ID counter
msg_id_counter = 1
pending_requests = {}

def log(msg):
    sys.stderr.write(f"[WhatsApp Node] {msg}\n")
    sys.stderr.flush()

class RequestHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # Suppress default HTTP logging to stdout/stderr
        pass
        
    def do_POST(self):
        global msg_id_counter
        
        if self.path == '/message':
            content_length = int(self.headers['Content-Length'])
            post_data = self.rfile.read(content_length).decode('utf-8')
            
            try:
                # Try parsing as JSON first
                payload = json.loads(post_data)
                body = payload.get('body', '')
                sender = payload.get('from', '+1234567890')
            except json.JSONDecodeError:
                # Fallback to form-encoded data
                parsed_data = parse_qs(post_data)
                body = parsed_data.get('body', [''])[0]
                sender = parsed_data.get('from', ['+1234567890'])[0]

            if not body:
                self.send_response(400)
                self.end_headers()
                self.wfile.write(b'Missing "body" parameter')
                return

            # Application Logic: Mappings handled in the Integration Node
            # Construct JSON-RPC Message
            message = {
                "jsonrpc": "2.0",
                "method": "whatsapp.message_received",
                "params": {
                    "from": sender,
                    "body": body
                },
                "id": msg_id_counter
            }
            msg_id_counter += 1
            # Register thread event to wait for Luotsi Core response
            event = threading.Event()
            response_data = {}
            pending_requests[message["id"]] = (event, response_data)
            
            # Emit to Luotsi Core
            log(f"HTTP Endpoint received prompt. Emitting to Core: {json.dumps(message)}")
            print(json.dumps(message))
            sys.stdout.flush()

            # Respond to HTTP client synchronously
            event.wait(timeout=120.0)
            
            if "reply" in response_data:
                self.send_response(200)
                self.send_header('Content-type', 'application/json')
                self.end_headers()
                self.wfile.write(json.dumps(response_data["reply"]).encode('utf-8'))
            else:
                self.send_response(504)
                self.send_header('Content-type', 'application/json')
                self.end_headers()
                self.wfile.write(json.dumps({"error": "Luotsi Core Timeout Processing Prompt"}).encode('utf-8'))
            
            pending_requests.pop(message["id"], None)
        else:
            self.send_response(404)
            self.end_headers()

def run_server():
    port = 8080
    server = HTTPServer(('0.0.0.0', port), RequestHandler)
    log(f"Listening for custom prompts via HTTP POST on http://0.0.0.0:{port}/message")
    server.serve_forever()

def main():
    log("Starting WhatsApp Integration Node...")
    
    # Start the HTTP server in a daemon thread
    server_thread = threading.Thread(target=run_server, daemon=True)
    server_thread.start()
    
    # Block and read responses from Core
    for line in sys.stdin:
        line_str = line.strip()
        log(f"Received from Core: {line_str}")
        try:
            resp = json.loads(line_str)
            if "id" in resp and resp["id"] in pending_requests:
                event, resp_data = pending_requests[resp["id"]]
                resp_data["reply"] = resp.get("result", resp)
                event.set()
        except json.JSONDecodeError:
            pass

if __name__ == "__main__":
    main()
