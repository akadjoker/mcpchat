
import json
import threading
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


@dataclass
class Reply:
    text: str = ""
    calls: list = field(default_factory=list)
    fragments: int = 3        # how many pieces text / arguments are cut into when streaming
    force_json: bool = False  # answer with plain JSON even when streaming was requested
    status: int = 200
    error_body: dict | None = None
    stall_after_first_chunk: bool = False
    usage: dict | None = None


def text_reply(text, **options):
    return Reply(text=text, **options)


def call_reply(calls, text="", **options):
    return Reply(text=text, calls=calls, **options)


def error_reply(status, message):
    return Reply(status=status, error_body={"error": {"message": message}})


def _arguments_text(arguments):
    return arguments if isinstance(arguments, str) else json.dumps(arguments)


def _pieces(text, count):
    size = max(1, -(-len(text) // max(1, count)))
    return [text[i:i + size] for i in range(0, len(text), size)] or [""]


def _stream_chunks(reply):
    def chunk(delta, finish=None):
        return {"choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}

    chunks = [chunk({"role": "assistant", "content": ""})]
    if reply.stall_after_first_chunk:
        chunks.append(chunk({"content": reply.text or "thinking"}))
        return chunks
    for piece in _pieces(reply.text, reply.fragments) if reply.text else []:
        chunks.append(chunk({"content": piece}))
    for index, (name, arguments) in enumerate(reply.calls):
        head = {"index": index, "id": f"call_{index}", "type": "function",
                "function": {"name": name, "arguments": ""}}
        chunks.append(chunk({"tool_calls": [head]}))
        for piece in _pieces(_arguments_text(arguments), reply.fragments):
            chunks.append(chunk({"tool_calls": [{"index": index, "function": {"arguments": piece}}]}))
    chunks.append(chunk({}, "tool_calls" if reply.calls else "stop"))
    if reply.usage:
        chunks.append({"choices": [], "usage": reply.usage})
    return chunks


def _json_body(reply):
    message = {"role": "assistant", "content": reply.text or None}
    if reply.calls:
        message["tool_calls"] = [
            {"id": f"call_{i}", "type": "function",
             "function": {"name": name, "arguments": _arguments_text(arguments)}}
            for i, (name, arguments) in enumerate(reply.calls)]
    body = {"choices": [{"index": 0, "message": message,
                         "finish_reason": "tool_calls" if reply.calls else "stop"}]}
    if reply.usage:
        body["usage"] = reply.usage
    return body


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_POST(self):
        server = self.server.owner
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length))
        with server.lock:
            server.requests.append({"body": body, "headers": dict(self.headers), "path": self.path})
            reply = server.script.pop(0) if server.script else error_reply(500, "script exhausted")
        try:
            self._respond(server, body, reply)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _respond(self, server, body, reply):
        if reply.status != 200:
            self._send_json(reply.status, reply.error_body or {})
        elif body.get("stream") and not reply.force_json:
            self._send_stream(server, reply)
        else:
            self._send_json(200, _json_body(reply))

    def _send_json(self, status, payload):
        data = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def _send_stream(self, server, reply):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        for chunk in _stream_chunks(reply):
            self._write(f"data: {json.dumps(chunk)}\n\n")
        if reply.stall_after_first_chunk:
            server.release.wait(10)
            return
        self._write("data: [DONE]\n\n")
        self.wfile.write(b"0\r\n\r\n")

    def _write(self, text):
        data = text.encode()
        self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
        self.wfile.flush()


class FakeLlmServer:
    def __init__(self):
        self.script = []
        self.requests = []
        self.lock = threading.Lock()
        self.release = threading.Event()  # lets a stalled stream end
        self._httpd = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        self._httpd.owner = self
        self._httpd.daemon_threads = True

    @property
    def base_url(self):
        return f"http://127.0.0.1:{self._httpd.server_address[1]}/v1"

    def start(self):
        threading.Thread(target=self._httpd.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True).start()
        return self

    def stop(self):
        self.release.set()
        self._httpd.shutdown()
        self._httpd.server_close()
