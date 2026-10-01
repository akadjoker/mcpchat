import io
import json

from fake_llm_server import call_reply, text_reply

from mcpchat.cli import main


def test_once_runs_a_turn_and_shows_the_tool_calls(llm_server, mcp_server, tmp_path):
    llm_server.script += [call_reply([("picture", {}), ("add", {"a": 1, "b": 2})]), text_reply("Done: 3.")]
    out = io.StringIO()
    code = main(["--config", str(tmp_path / "none.json"), "--server", mcp_server.url, "--base-url", llm_server.base_url,
                 "--model", "m", "--images", str(tmp_path / "img"), "--once", "go"], out)
    text = out.getvalue()
    assert code == 0, text
    assert "● 127_0_0_1: Fake Server 9 - 5 tools, MCP 2025-11-25" in text
    assert "→ picture {}" in text and "→ add {\"a\":1,\"b\":2}" in text
    assert "[image: " in text and list((tmp_path / "img").glob("*_picture.png"))
    assert "Done: 3." in text


def test_configuration_file_and_problems(llm_server, mcp_server, tmp_path):
    out = io.StringIO()
    assert main(["--config", str(tmp_path / "c.json"), "--init"], out) == 0
    example = json.loads((tmp_path / "c.json").read_text())
    assert example["servers"][0]["url"].endswith("/mcp") and example["profiles"]

    config = {"current_profile": "fake", "servers": [{"name": "fake", "url": mcp_server.url}],
              "profiles": [{"name": "fake", "base_url": llm_server.base_url, "model": "m"}]}
    (tmp_path / "ok.json").write_text(json.dumps(config))
    llm_server.script.append(text_reply("hello"))
    out = io.StringIO()
    assert main(["--config", str(tmp_path / "ok.json"), "--once", "hi"], out) == 0
    assert "hello" in out.getvalue()

    (tmp_path / "bad.json").write_text(json.dumps({"servers": [{"name": "x"}], "profiles": [], "confirm": "maybe"}))
    out = io.StringIO()
    assert main(["--config", str(tmp_path / "bad.json"), "--once", "hi"], out) == 2
    problems = out.getvalue()
    assert "give either 'url' or 'command'" in problems and "'confirm' must be" in problems and "no LLM profile" in problems

    (tmp_path / "typo.json").write_text(json.dumps({"servers": [{"name": "x", "uri": "http://a"}]}))
    out = io.StringIO()
    assert main(["--config", str(tmp_path / "typo.json")], out) == 2
    assert "unknown field(s) uri" in out.getvalue()


def test_no_tools_anywhere(llm_server, tmp_path):
    out = io.StringIO()
    code = main(["--config", str(tmp_path / "none.json"), "--server", "http://127.0.0.1:9/mcp", "--base-url",
                 llm_server.base_url, "--model", "m", "--once", "hi"], out)
    assert code == 3 and "○ 127_0_0_1: cannot reach" in out.getvalue()
