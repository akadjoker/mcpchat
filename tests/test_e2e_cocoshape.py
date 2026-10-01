"""mcpchat against a real CocoShape editor over MCP, with a scripted model.

Run with COCOSHAPE_BIN=/path/to/cocoshape (needs a display, or xvfb-run): pytest -m e2e
"""

import io
import json
import os
import socket
import subprocess
import time
import urllib.request

import pytest
from fake_llm_server import call_reply, text_reply

from mcpchat.cli import main

pytestmark = pytest.mark.e2e
BINARY = os.environ.get("COCOSHAPE_BIN", "")


@pytest.fixture
def editor():
    if not BINARY:
        pytest.skip("set COCOSHAPE_BIN to a cocoshape binary")
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    process = subprocess.Popen([BINARY, "--api-port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    url = f"http://127.0.0.1:{port}"
    for _ in range(120):
        try:
            urllib.request.urlopen(url + "/api/health", timeout=1).close()
            break
        except OSError:
            time.sleep(0.25)
    else:
        process.kill()
        pytest.fail("the editor did not start")
    yield url
    process.terminate()
    process.wait(timeout=10)


def status(url):
    with urllib.request.urlopen(urllib.request.Request(url + "/api/commands/get_status", data=b"{}",
                                                       headers={"Content-Type": "application/json"})) as response:
        return json.loads(response.read())["result"]


def test_a_model_builds_in_the_editor(editor, llm_server, tmp_path):
    llm_server.script += [
        call_reply([("add_primitive", {"type": "box", "name": "body", "size": [2, 1, 1], "color": "#aa3300"}),
                    ("add_primitive", {"type": "cylinder", "name": "mast", "radius": 0.1, "height": 2,
                                       "position": [0, 1.5, 0]})]),
        call_reply([("screenshot", {"width": 160, "height": 120}), ("get_status", {})]),
        call_reply([("bevel", {"width": -1})]),
        text_reply("Built a box with a mast."),
    ]
    out = io.StringIO()
    code = main(["--config", str(tmp_path / "none.json"), "--server", editor + "/mcp", "--base-url",
                 llm_server.base_url, "--model", "m", "--vision", "--images", str(tmp_path / "img"),
                 "--once", "build a box with a mast"], out)
    text = out.getvalue()
    assert code == 0, text
    assert "CocoShape" in text and "MCP 2025-11-25" in text
    assert [p["name"] for p in status(editor)["parts"]] == ["body", "mast"]
    assert list((tmp_path / "img").glob("*_screenshot.png"))
    assert "✗ ERROR invalid_params" in text
    system = llm_server.requests[0]["body"]["messages"][0]["content"]
    assert "# CocoShape" in system and "Units are metres" in system
    looked = llm_server.requests[2]["body"]["messages"][-1]
    assert looked["role"] == "user" and looked["content"][1]["image_url"]["url"].startswith("data:image/png")
