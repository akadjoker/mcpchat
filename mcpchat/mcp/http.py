"""Streamable HTTP: each message is a POST; the answer comes back as one JSON body or as an event stream."""

import json
import urllib.error
import urllib.request

from .client import McpTransportError


class HttpTransport:
    def __init__(self, url, headers=None, timeout=300.0):
        self.url = url
        self.headers = dict(headers or {})
        self.timeout = timeout
        self.session_id = None
        self.protocol_version = None

    def set_protocol_version(self, version):
        self.protocol_version = version

    def send(self, message):
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", **self.headers}
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        if self.protocol_version:
            headers["MCP-Protocol-Version"] = self.protocol_version
        body = json.dumps(message, separators=(",", ":")).encode("utf-8")
        request = urllib.request.Request(self.url, data=body, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                session = response.headers.get("Mcp-Session-Id")
                if session:
                    self.session_id = session
                if response.status == 202:
                    return None
                kind = (response.headers.get("Content-Type") or "").split(";")[0].strip().lower()
                if kind == "text/event-stream":
                    return _read_event_stream(response, message.get("id"))
                raw = response.read()
                if not raw:
                    return None
                return _parse(raw)
        except urllib.error.HTTPError as error:
            raw = error.read()
            try:
                reply = json.loads(raw)
                if isinstance(reply, dict) and reply.get("jsonrpc") == "2.0" and "error" in reply:
                    return reply
            except ValueError:
                pass
            if error.code == 404 and self.session_id:
                raise McpTransportError("the server ended the session; reconnect", 404) from None
            hint = " (check the token)" if error.code in (401, 403) else ""
            raise McpTransportError(f"{self.url} answered HTTP {error.code}{hint}: {_snippet(raw)}", error.code) from None
        except (urllib.error.URLError, OSError) as error:
            reason = getattr(error, "reason", error)
            raise McpTransportError(f"cannot reach {self.url}: {reason}") from None

    def close(self):
        if not self.session_id:
            return
        headers = {"Mcp-Session-Id": self.session_id, **self.headers}
        request = urllib.request.Request(self.url, headers=headers, method="DELETE")
        try:
            urllib.request.urlopen(request, timeout=5).close()
        except (urllib.error.URLError, OSError):
            pass  # the server may not allow ending sessions; nothing more to do
        self.session_id = None


def _parse(raw):
    try:
        return json.loads(raw)
    except ValueError:
        raise McpTransportError(f"the server answered with something that is not JSON: {_snippet(raw)}") from None


def _read_event_stream(response, request_id):
    """The JSON-RPC response to `request_id` out of an SSE stream; server notifications on the way are skipped."""
    data = []
    for raw_line in response:
        line = raw_line.decode("utf-8", "replace").rstrip("\r\n")
        if line.startswith("data:"):
            data.append(line[5:].lstrip(" "))
            continue
        if line or not data:
            continue
        event = _parse("\n".join(data).encode("utf-8"))
        data = []
        if isinstance(event, dict) and event.get("id") == request_id and ("result" in event or "error" in event):
            return event
    if data:
        event = _parse("\n".join(data).encode("utf-8"))
        if isinstance(event, dict) and event.get("id") == request_id:
            return event
    raise McpTransportError("the event stream ended without an answer")


def _snippet(raw):
    text = raw.decode("utf-8", "replace") if isinstance(raw, bytes) else str(raw)
    return text[:200]
