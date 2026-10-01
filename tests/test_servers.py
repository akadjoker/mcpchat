from conftest import FAKE_STDIO_SERVER
from fake_mcp_server import FakeMcpHttpServer

from mcpchat.config import ServerConfig
from mcpchat.servers import ServerHub, exposed_name


def test_one_server_keeps_tool_names(mcp_server):
    hub = ServerHub([ServerConfig("fake", url=mcp_server.url)]).connect()
    assert sorted(hub.tools) == ["add", "broken", "paint", "picture", "wipe"]
    assert hub.tools["add"].read_only and hub.tools["wipe"].destructive and not hub.tools["paint"].destructive
    assert hub.call("add", {"a": 1, "b": 1}).structured == {"sum": 2}
    assert hub.instructions() == [("Fake Server", "Use the tools.")]
    hub.close()


def test_several_servers_are_prefixed_and_a_dead_one_is_reported(mcp_server):
    hub = ServerHub([
        ServerConfig("web", url=mcp_server.url),
        ServerConfig("local", command=FAKE_STDIO_SERVER[0], args=FAKE_STDIO_SERVER[1:]),
        ServerConfig("gone", url="http://127.0.0.1:9/mcp", timeout=2),
        ServerConfig("off", url=mcp_server.url, enabled=False),
    ]).connect()
    assert "web__add" in hub.tools and "local__add" in hub.tools and len(hub.tools) == 10
    assert hub.call("local__add", {"a": 2, "b": 3}).structured == {"sum": 5}
    states = {server.name: (server.client is not None, server.error) for server in hub.servers}
    assert states["web"][0] and states["local"][0]
    assert not states["gone"][0] and "cannot reach" in states["gone"][1]
    assert "off" not in states
    hub.close()


def test_token_comes_from_the_named_environment_variable(monkeypatch):
    server = FakeMcpHttpServer(token="t0ken").start()
    try:
        monkeypatch.setenv("FAKE_TOKEN", "t0ken")
        hub = ServerHub([ServerConfig("locked", url=server.url, token_env="FAKE_TOKEN")]).connect()
        assert hub.servers[0].client is not None and hub.tools
        monkeypatch.setenv("FAKE_TOKEN", "wrong")
        hub = ServerHub([ServerConfig("locked", url=server.url, token_env="FAKE_TOKEN")]).connect()
        assert "401" in hub.servers[0].error
    finally:
        server.stop()


def test_exposed_names_fit_function_name_rules():
    assert exposed_name("my server", "do.thing", True) == "my_server__do_thing"
    long = exposed_name("s" * 40, "t" * 40, True)
    assert len(long) == 64 and long != exposed_name("s" * 40, "t" * 39 + "u", True)
