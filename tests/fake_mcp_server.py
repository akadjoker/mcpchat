"""A small MCP server for the tests: a few tools, served over Streamable HTTP (JSON or event-stream replies) or over
stdio when run as a script."""

import base64
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PNG = base64.b64encode(bytes.fromhex(
    "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d4944415478da63f8cfc0f01f0005000201"
    "c1e1a5c40000000049454e44ae426082")).decode()

TOOLS = [
    {"name": "add", "description": "Adds two numbers.",
     "inputSchema": {"type": "object", "properties": {"a": {"type": "number"}, "b": {"type": "number"}},
                     "required": ["a", "b"]},
     "annotations": {"readOnlyHint": True}},
    {"name": "wipe", "description": "Deletes everything.", "inputSchema": {"type": "object", "properties": {}},
     "annotations": {"readOnlyHint": False, "destructiveHint": True}},
    {"name": "paint", "description": "Changes the colour.", "inputSchema": {"type": "object", "properties": {}},
     "annotations": {"readOnlyHint": False, "destructiveHint": False}},
    {"name": "picture", "description": "Returns an image.", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "broken", "description": "Always fails.", "inputSchema": {"type": "object", "properties": {}}},
]


class McpLogic:
    def __init__(self, instructions="Use the tools.", page_size=0):
        self.instructions = instructions
        self.page_size = page_size
        self.calls = []
        self.notifications = []

    def handle(self, message):
        if "method" not in message:
            return None
        if "id" not in message:
            self.notifications.append(message["method"])
            return None
        method, params, mid = message["method"], message.get("params") or {}, message["id"]
        if method == "initialize":
            return self._ok(mid, {"protocolVersion": "2025-11-25", "capabilities": {"tools": {}},
                                  "serverInfo": {"name": "fake", "title": "Fake Server", "version": "9"},
                                  "instructions": self.instructions})
        if method == "ping":
            return self._ok(mid, {})
        if method == "tools/list":
            if not self.page_size:
                return self._ok(mid, {"tools": TOOLS})
            start = int(params.get("cursor") or 0)
            page = {"tools": TOOLS[start:start + self.page_size]}
            if start + self.page_size < len(TOOLS):
                page["nextCursor"] = str(start + self.page_size)
            return self._ok(mid, page)
        if method == "tools/call":
            name, arguments = params.get("name"), params.get("arguments") or {}
            self.calls.append((name, arguments))
            if name == "add":
                total = arguments["a"] + arguments["b"]
                return self._ok(mid, {"content": [{"type": "text", "text": json.dumps({"sum": total})}],
                                      "structuredContent": {"sum": total}, "isError": False})
            if name in ("wipe", "paint"):
                return self._ok(mid, {"content": [{"type": "text", "text": f"{name} done"}]})
            if name == "picture":
                return self._ok(mid, {"content": [{"type": "text", "text": "here"},
                                                  {"type": "image", "data": PNG, "mimeType": "image/png"}]})
            if name == "broken":
                return self._ok(mid, {"content": [{"type": "text", "text": "it broke"}], "isError": True})
            return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": f"Unknown tool: {name}"}}
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": f"Method not found: {method}"}}

    @staticmethod
    def _ok(mid, result):
        return {"jsonrpc": "2.0", "id": mid, "result": result}


class _Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        server = self.server.owner
        server.requests.append({key.lower(): value for key, value in self.headers.items()})
        if server.token and self.headers.get("Authorization") != f"Bearer {server.token}":
            self._send(401, "application/json", b'{"error":"unauthorized"}')
            return
        message = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        reply = server.logic.handle(message)
        if reply is None:
            self._send(202, None, b"")
            return
        if server.sse:
            note = json.dumps({"jsonrpc": "2.0", "method": "notifications/progress", "params": {"progress": 1}})
            body = f"event: message\ndata: {note}\n\nevent: message\ndata: {json.dumps(reply)}\n\n".encode()
            self._send(200, "text/event-stream", body, {"Mcp-Session-Id": "s-1"})
        else:
            self._send(200, "application/json", json.dumps(reply).encode(), {"Mcp-Session-Id": "s-1"})

    def do_DELETE(self):
        self.server.owner.deleted = True
        self._send(200, None, b"")

    def _send(self, status, kind, body, headers=None):
        self.send_response(status)
        if kind:
            self.send_header("Content-Type", kind)
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class FakeMcpHttpServer:
    def __init__(self, sse=False, token="", page_size=0, instructions="Use the tools."):
        self.logic = McpLogic(instructions, page_size)
        self.sse = sse
        self.token = token
        self.requests = []
        self.deleted = False
        self._httpd = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        self._httpd.owner = self

    @property
    def url(self):
        return f"http://127.0.0.1:{self._httpd.server_address[1]}/mcp"

    def start(self):
        threading.Thread(target=self._httpd.serve_forever, daemon=True).start()
        return self

    def stop(self):
        self._httpd.shutdown()
        self._httpd.server_close()


def serve_stdio():
    logic = McpLogic("Stdio instructions.")
    print("fake stdio server ready", file=sys.stderr, flush=True)
    for line in sys.stdin:
        if not line.strip():
            continue
        message = json.loads(line)
        if message.get("method") == "tools/call" and (message.get("params") or {}).get("name") == "crash":
            print("crashing on purpose", file=sys.stderr, flush=True)
            sys.exit(3)
        reply = logic.handle(message)
        if reply is not None:
            sys.stdout.write(json.dumps(reply) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    serve_stdio()
