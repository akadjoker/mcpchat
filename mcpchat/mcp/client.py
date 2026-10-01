"""An MCP client over any transport: the initialize handshake, tools/list and tools/call.

It speaks the handshake revisions (2024-11-05 to 2025-11-25). Servers that only speak the stateless 2026-07-28
revision are out of reach until that is added.
"""

import itertools
import threading
from dataclasses import dataclass, field

LATEST_VERSION = "2025-11-25"
SUPPORTED_VERSIONS = ("2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25")


class McpError(Exception):
    """A JSON-RPC error the server answered with."""

    def __init__(self, code, message, data=None):
        super().__init__(f"{message} (code {code})")
        self.code = code
        self.message = message
        self.data = data


class McpTransportError(Exception):
    """The server could not be reached, or answered with something that is not JSON-RPC."""

    def __init__(self, message, http_status=None):
        super().__init__(message)
        self.http_status = http_status


@dataclass
class ToolResult:
    content: list = field(default_factory=list)
    is_error: bool = False
    structured: dict | None = None

    def text(self):
        parts = [block.get("text", "") for block in self.content if block.get("type") == "text"]
        for block in self.content:
            if block.get("type") == "resource" and isinstance(block.get("resource"), dict):
                parts.append(block["resource"].get("text", ""))
        return "\n".join(part for part in parts if part)

    def images(self):
        return [block for block in self.content if block.get("type") == "image" and block.get("data")]


class McpClient:
    def __init__(self, transport, client_name="mcpchat", client_version="0.1.0"):
        self.transport = transport
        self.client_name = client_name
        self.client_version = client_version
        self.protocol_version = None
        self.server_info = {}
        self.capabilities = {}
        self.instructions = ""
        self._ids = itertools.count(1)
        self._lock = threading.Lock()

    def initialize(self):
        result = self.request("initialize", {
            "protocolVersion": LATEST_VERSION,
            "capabilities": {},
            "clientInfo": {"name": self.client_name, "version": self.client_version},
        })
        version = result.get("protocolVersion")
        if version not in SUPPORTED_VERSIONS:
            raise McpTransportError(f"the server wants MCP protocol version {version!r}, which this client does not speak")
        self.protocol_version = version
        self.server_info = result.get("serverInfo") or {}
        self.capabilities = result.get("capabilities") or {}
        self.instructions = result.get("instructions") or ""
        self.transport.set_protocol_version(version)
        self.notify("notifications/initialized")
        return result

    def list_tools(self):
        if "tools" not in self.capabilities:
            return []
        tools, cursor = [], None
        for _ in range(1000):  # a server that never stops paging is a bug, not a reason to hang
            params = {"cursor": cursor} if cursor else {}
            result = self.request("tools/list", params)
            tools.extend(result.get("tools") or [])
            cursor = result.get("nextCursor")
            if not cursor:
                break
        return tools

    def call_tool(self, name, arguments=None):
        result = self.request("tools/call", {"name": name, "arguments": arguments or {}})
        structured = result.get("structuredContent")
        return ToolResult(result.get("content") or [], bool(result.get("isError")),
                          structured if isinstance(structured, dict) else None)

    def ping(self):
        self.request("ping", {})

    def request(self, method, params=None):
        with self._lock:
            request_id = next(self._ids)
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        reply = self.transport.send(message)
        if not isinstance(reply, dict) or reply.get("id") != request_id:
            raise McpTransportError(f"no answer to {method}")
        if "error" in reply:
            error = reply["error"] or {}
            raise McpError(error.get("code"), error.get("message", "error"), error.get("data"))
        return reply.get("result") or {}

    def notify(self, method, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self.transport.send(message)

    def close(self):
        self.transport.close()
