import sys
from pathlib import Path

import pytest

TESTS = Path(__file__).parent
sys.path.insert(0, str(TESTS))  # the fake servers live next to the tests

from fake_llm_server import FakeLlmServer  # noqa: E402
from fake_mcp_server import FakeMcpHttpServer  # noqa: E402

FAKE_STDIO_SERVER = [sys.executable, str(TESTS / "fake_mcp_server.py")]


@pytest.fixture
def llm_server():
    server = FakeLlmServer().start()
    yield server
    server.stop()


@pytest.fixture
def mcp_server():
    server = FakeMcpHttpServer().start()
    yield server
    server.stop()


@pytest.fixture
def mcp_sse_server():
    server = FakeMcpHttpServer(sse=True).start()
    yield server
    server.stop()
