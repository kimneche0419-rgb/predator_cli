#!/usr/bin/env python3
"""Mock LSP server for client tests: initialize, didChange, hover."""
import sys, json

def read_msg():
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if line in (b"", b"\r\n"):
            break
        k, _, v = line.decode().partition(":")
        headers[k.strip().lower()] = v.strip()
    n = int(headers.get("content-length", 0))
    return json.loads(sys.stdin.buffer.read(n))

def send(obj):
    b = json.dumps(obj).encode()
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(b) + b)
    sys.stdout.buffer.flush()

while True:
    try:
        msg = read_msg()
    except Exception:
        break
    m = msg.get("method")
    if m == "initialize":
        send({"jsonrpc": "2.0", "id": msg["id"], "result": {"capabilities": {"hoverProvider": True}}})
    elif m == "textDocument/hover":
        send({"jsonrpc": "2.0", "id": msg["id"],
              "result": {"contents": {"kind": "plaintext", "value": "int main(void)"}}})
    elif m == "textDocument/didChange":
        text = msg["params"]["contentChanges"][0]["text"]
        if "BAD" in text:
            send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
                  "params": {"uri": msg["params"]["textDocument"]["uri"],
                             "diagnostics": [{"message": "use of undeclared identifier", "severity": 1}]}})
    elif m == "shutdown":
        send({"jsonrpc": "2.0", "id": msg["id"], "result": None})
    elif m == "exit":
        sys.exit(0)
