#!/usr/bin/env python3
"""Mock Ollama /api/chat server for the agent tool loop test.

Serves ONE scripted conversation on 127.0.0.1:<port> (default 11436):
  1. native tool_calls: run_shell creates created.txt and echoes a marker
  2. native tool_calls: edit_file replaces "world" with "agent"
  3. native tool_calls: read_file reads the file back
  4. no native tool_calls; assistant text embeds a fenced ```json block that
     tries to edit /etc/passwd (must be rejected by the agent's path sandbox)
  5. native tool_calls: edit_file with a relative "../" escape (rejected)
  6. final answer text (streamed as plain content)

Responses are NDJSON, split across several lines to exercise incremental
client-side parsing.
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

FALLBACK_TEXT = (
    "I cannot edit files outside the workspace.\n"
    "```json\n"
    '{"tool": "edit_file", "args": {"path": "/etc/passwd", '
    '"old_string": "root", "new_string": "pwn"}}\n'
    "```\n"
)


def user_turn():
    return {"role": "user", "content": "scripted goal"}


def tool_line(name, args_json):
    return {"message": {"role": "assistant", "content": "",
                        "tool_calls": [{"function": {"name": name,
                                                     "arguments": args_json}}]},
            "done": False}


def done_line(content=""):
    return {"message": {"role": "assistant", "content": content}, "done": True}


STEPS = [
    # 1: create the file via run_shell, echo a marker to verify output capture
    [tool_line("run_shell",
               "{\"cmd\": \"printf 'hello world' > created.txt && echo created-by-agent\"}"),
     done_line()],
    # 2: edit_file replacement
    [tool_line("edit_file",
               "{\"path\": \"created.txt\", \"old_string\": \"world\", "
               "\"new_string\": \"agent\"}"),
     done_line()],
    # 3: read the file back
    [tool_line("read_file", "{\"path\": \"created.txt\"}"),
     done_line()],
    # 4: fallback fenced-block tool call (split across two chunks)
    [{"message": {"role": "assistant", "content": FALLBACK_TEXT[:40]}, "done": False},
     {"message": {"role": "assistant", "content": FALLBACK_TEXT[40:]}, "done": False},
     done_line()],
    # 5: relative ../ escape attempt (native tool_calls)
    [tool_line("edit_file",
               "{\"path\": \"../outside.txt\", \"old_string\": \"x\", "
               "\"new_string\": \"y\"}"),
     done_line()],
    # 6: final answer, split over two chunks
    [{"message": {"role": "assistant",
                  "content": "Done: created.txt now says hello agent"}, "done": False},
     done_line(".")],
]


class H(BaseHTTPRequestHandler):
    step = 0

    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(n))
        if (self.path != "/api/chat" or "model" not in body
                or "messages" not in body or "tools" not in body
                or body.get("stream") is not True):
            self._send({"error": "bad request"}, 400)
            return
        if body.get("messages") == []:  # readiness ping: never consumes a step
            self._send({"ok": True}, 200)
            return
        if H.step >= len(STEPS):
            self._send({"error": "script exhausted"}, 500)
            return
        lines = STEPS[H.step]
        H.step += 1
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.end_headers()
        for obj in lines:
            self.wfile.write(json.dumps(obj, separators=(",", ":")).encode() + b"\n")

    def _send(self, obj, status):
        b = json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 11436
    HTTPServer(("127.0.0.1", port), H).serve_forever()
