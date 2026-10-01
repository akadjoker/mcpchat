"""Every configured MCP server behind one tool list: the model sees `tool` when there is one server and
`server__tool` when there are several."""

import hashlib
import os
import re
from dataclasses import dataclass

from .mcp import HttpTransport, McpClient, McpError, McpTransportError, StdioTransport

_NAME_LIMIT = 64  # what OpenAI-compatible servers accept for a function name
_BAD_CHARACTERS = re.compile(r"[^A-Za-z0-9_-]")


@dataclass
class ExposedTool:
    name: str        # what the model calls
    server: str
    tool: dict       # the MCP Tool

    @property
    def annotations(self):
        return self.tool.get("annotations") or {}

    @property
    def read_only(self):
        return self.annotations.get("readOnlyHint") is True

    @property
    def destructive(self):
        # MCP's default for a tool that is not read-only is destructive; the policy decides whether that default counts.
        return self.annotations.get("destructiveHint") is True


@dataclass
class ConnectedServer:
    name: str
    client: McpClient | None = None
    error: str = ""
    tools: list = None


def _token(server, secrets):
    if not server.token_env:
        return ""
    if secrets is not None:
        from .secrets import server_token_account
        return secrets.resolve(server_token_account(server.name), server.token_env).value
    return os.environ.get(server.token_env, "")


def open_transport(server, secrets=None):
    if server.url:
        headers = dict(server.headers)
        token = _token(server, secrets)
        if token and "Authorization" not in headers:
            headers["Authorization"] = f"Bearer {token}"
        return HttpTransport(server.url, headers, server.timeout)
    env = {**os.environ, **{str(k): str(v) for k, v in server.env.items()}}
    return StdioTransport(server.command, server.args, env, timeout=server.timeout)


def exposed_name(server, tool, prefixed):
    raw = f"{server}__{tool}" if prefixed else tool
    name = _BAD_CHARACTERS.sub("_", raw)
    if len(name) > _NAME_LIMIT:
        digest = hashlib.sha1(raw.encode("utf-8")).hexdigest()[:8]
        name = name[:_NAME_LIMIT - 9] + "_" + digest
    return name


class ServerHub:
    def __init__(self, servers, secrets=None, client_name="mcpchat", client_version="0.1.0"):
        self._configs = [s for s in servers if s.enabled]
        self._secrets = secrets
        self._client_name = client_name
        self._client_version = client_version
        self.servers = []
        self.tools = {}

    def connect(self):
        """Connects to every enabled server; a server that fails is reported, the others still work."""
        self.close()
        for config in self._configs:
            entry = ConnectedServer(config.name, tools=[])
            try:
                client = McpClient(open_transport(config, self._secrets), self._client_name, self._client_version)
                client.initialize()
                entry.tools = client.list_tools()
                entry.client = client
            except (McpError, McpTransportError) as error:
                entry.error = str(error)
            self.servers.append(entry)
        self._index()
        return self

    def _index(self):
        self.tools = {}
        live = [s for s in self.servers if s.client]
        prefixed = len(live) > 1
        for server in live:
            for tool in server.tools:
                name = exposed_name(server.name, tool["name"], prefixed)
                self.tools[name] = ExposedTool(name, server.name, tool)

    def instructions(self):
        parts = []
        for server in self.servers:
            if server.client and server.client.instructions.strip():
                title = server.client.server_info.get("title") or server.client.server_info.get("name") or server.name
                parts.append((title, server.client.instructions.strip()))
        return parts

    def call(self, exposed, arguments):
        tool = self.tools[exposed]
        server = next(s for s in self.servers if s.name == tool.server)
        return server.client.call_tool(tool.tool["name"], arguments)

    def close(self):
        for server in self.servers:
            if server.client:
                server.client.close()
        self.servers = []
        self.tools = {}
