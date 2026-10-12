#!/usr/bin/env python3
"""Mock Ollama server for client tests: /api/tags, /api/generate (NDJSON)."""
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, obj, status=200):
        b = json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path == "/api/tags":
            self._send({"models": [
                {"name": "qwen2.5-coder:1.5b", "size": 986000000000},
                {"name": "qwen2.5-coder:3b", "size": 1900000000000},
            ]})
        else:
            self._send({"error": "not found"}, 404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(n))
        if self.path != "/api/generate" or "model" not in body or "prompt" not in body:
            self._send({"error": "bad request"}, 400)
            return
        if body["model"].startswith("nope"):
            self._send({"error": "model not found"}, 404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.end_headers()
        for chunk in body["prompt"].split():
            self.wfile.write(json.dumps({"response": chunk + " ", "done": False}, separators=(",", ":")).encode() + b"\n")
        self.wfile.write(json.dumps({"response": "", "done": True}, separators=(",", ":")).encode() + b"\n")

if __name__ == "__main__":
    port = int(sys.argv[1])
    HTTPServer(("127.0.0.1", port), H).serve_forever()
