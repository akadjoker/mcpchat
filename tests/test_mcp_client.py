import sys

import pytest
from conftest import FAKE_STDIO_SERVER
from fake_mcp_server import FakeMcpHttpServer

from mcpchat.mcp import HttpTransport, McpClient, McpError, McpTransportError, StdioTransport


def connect_http(url, **options):
    client = McpClient(HttpTransport(url, **options))
    client.initialize()
    return client


def test_http_handshake_tools_and_calls(mcp_server):
    client = connect_http(mcp_server.url)
    assert client.protocol_version == "2025-11-25"
    assert client.server_info["title"] == "Fake Server"
    assert client.instructions == "Use the tools."
    assert mcp_server.logic.notifications == ["notifications/initialized"]
    # After the handshake every request names the protocol version and echoes the session.
    names = [tool["name"] for tool in client.list_tools()]
    assert names == ["add", "wipe", "paint", "picture", "broken"]
    assert mcp_server.requests[-1]["mcp-protocol-version"] == "2025-11-25"
    assert mcp_server.requests[-1]["mcp-session-id"] == "s-1"
    assert "text/event-stream" in mcp_server.requests[-1]["accept"]

    result = client.call_tool("add", {"a": 2, "b": 40})
    assert not result.is_error and result.structured == {"sum": 42} and '"sum": 42' in result.text()
    picture = client.call_tool("picture")
    assert picture.text() == "here" and picture.images()[0]["mimeType"] == "image/png"
    assert client.call_tool("broken").is_error
    with pytest.raises(McpError) as raised:
        client.call_tool("nope")
    assert raised.value.code == -32602
    client.close()
    assert mcp_server.deleted


def test_event_stream_replies(mcp_sse_server):
    client = connect_http(mcp_sse_server.url)
    assert client.call_tool("add", {"a": 1, "b": 2}).structured == {"sum": 3}


def test_paginated_tool_list():
    server = FakeMcpHttpServer(page_size=2).start()
    try:
        assert len(connect_http(server.url).list_tools()) == 5
    finally:
        server.stop()


def test_token_and_unreachable_server():
    server = FakeMcpHttpServer(token="secret").start()
    try:
        with pytest.raises(McpTransportError) as raised:
            connect_http(server.url)
        assert raised.value.http_status == 401 and "token" in str(raised.value)
        assert connect_http(server.url, headers={"Authorization": "Bearer secret"}).list_tools()
    finally:
        server.stop()
    with pytest.raises(McpTransportError, match="cannot reach"):
        connect_http("http://127.0.0.1:9/mcp", timeout=2)


def test_stdio_server():
    client = McpClient(StdioTransport(FAKE_STDIO_SERVER[0], FAKE_STDIO_SERVER[1:], timeout=10))
    client.initialize()
    assert client.instructions == "Stdio instructions."
    assert len(client.list_tools()) == 5
    assert client.call_tool("add", {"a": 5, "b": 5}).structured == {"sum": 10}
    client.ping()
    with pytest.raises(McpTransportError, match="exited with code 3"):
        client.call_tool("crash")
    assert "crashing on purpose" in "\n".join(client.transport.stderr_tail)
    client.close()


def test_stdio_command_that_does_not_exist():
    with pytest.raises(McpTransportError, match="cannot start"):
        StdioTransport("/no/such/server")


def test_a_server_that_wants_an_unknown_version(monkeypatch, mcp_server):
    original = mcp_server.logic.handle

    def future(message):
        reply = original(message)
        if message.get("method") == "initialize":
            reply["result"]["protocolVersion"] = "2099-01-01"
        return reply

    mcp_server.logic.handle = future
    with pytest.raises(McpTransportError, match="2099-01-01"):
        connect_http(mcp_server.url)
