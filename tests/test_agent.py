import base64

from fake_llm_server import call_reply, error_reply, text_reply
from fake_mcp_server import PNG

from mcpchat.agent import Agent, AgentConfig, AgentListener
from mcpchat.config import ServerConfig
from mcpchat.llm.openai_compat import OpenAICompatProvider
from mcpchat.servers import ServerHub


class Recorder(AgentListener):
    def __init__(self):
        self.calls, self.results, self.errors, self.text = [], [], [], ""

    def on_text_delta(self, text):
        self.text += text

    def on_tool_call(self, call_id, name, arguments):
        self.calls.append((name, arguments))

    def on_tool_result(self, call_id, name, text, is_error, images):
        self.results.append((name, text, is_error, images))

    def on_error(self, message):
        self.errors.append(message)


def make_agent(llm_server, mcp_server, vision=False, confirm=None, policy="destructive"):
    hub = ServerHub([ServerConfig("fake", url=mcp_server.url)]).connect()
    recorder = Recorder()
    provider = OpenAICompatProvider(llm_server.base_url, "m", vision=vision)
    return Agent(provider, hub, recorder, AgentConfig(confirm=policy), confirm), recorder


def test_tool_round_trip_and_final_answer(llm_server, mcp_server):
    llm_server.script += [call_reply([("add", {"a": 2, "b": 40})]), text_reply("The sum is 42.")]
    agent, recorder = make_agent(llm_server, mcp_server)
    result = agent.run("add 2 and 40")
    assert result.reason == "done" and result.steps == 2
    assert recorder.calls == [("add", {"a": 2, "b": 40})]
    assert recorder.text.endswith("The sum is 42.")
    assert mcp_server.logic.calls == [("add", {"a": 2, "b": 40})]
    first = llm_server.requests[0]["body"]
    assert [t["function"]["name"] for t in first["tools"]] == ["add", "wipe", "paint", "picture", "broken"]
    assert "# Fake Server\nUse the tools." in first["messages"][0]["content"]
    second = llm_server.requests[1]["body"]["messages"]
    assert second[-1]["role"] == "tool" and '"sum": 42' in second[-1]["content"]


def test_tool_errors_reach_the_model(llm_server, mcp_server):
    llm_server.script += [call_reply([("broken", {}), ("nope", {})]), text_reply("Could not do it.")]
    agent, recorder = make_agent(llm_server, mcp_server)
    assert agent.run("try").reason == "done"
    assert recorder.results[0][1:3] == ("it broke", True)
    assert recorder.results[1][2] and "there is no tool 'nope'" in recorder.results[1][1]
    tool_messages = [m for m in llm_server.requests[1]["body"]["messages"] if m["role"] == "tool"]
    assert len(tool_messages) == 2


def test_destructive_tools_ask_first(llm_server, mcp_server):
    asked = []
    llm_server.script += [call_reply([("wipe", {}), ("paint", {})]), text_reply("ok")]
    agent, recorder = make_agent(llm_server, mcp_server, confirm=lambda name, args: asked.append(name) or False)
    agent.run("clean up")
    assert asked == ["wipe"]  # paint is not destructive under the default policy
    assert [c[0] for c in mcp_server.logic.calls] == ["paint"]
    assert "declined" in recorder.results[0][1]

    llm_server.script += [call_reply([("paint", {})]), text_reply("ok")]
    agent, _ = make_agent(llm_server, mcp_server, confirm=lambda name, args: asked.append(name) or True, policy="writes")
    agent.run("paint")
    assert asked[-1] == "paint"


def test_images_go_to_a_model_that_can_see(llm_server, mcp_server):
    llm_server.script += [call_reply([("picture", {})]), text_reply("I see it.")]
    agent, recorder = make_agent(llm_server, mcp_server, vision=True)
    agent.run("look")
    assert recorder.results[0][3] == [("image/png", base64.b64decode(PNG))]
    last = llm_server.requests[1]["body"]["messages"][-1]
    assert last["role"] == "user" and last["content"][1]["image_url"]["url"].startswith("data:image/png;base64,")

    llm_server.script += [call_reply([("picture", {})]), text_reply("ok")]
    blind, _ = make_agent(llm_server, mcp_server, vision=False)
    blind.run("look")
    assert "cannot see images" in llm_server.requests[-1]["body"]["messages"][-1]["content"]


def test_unreachable_server_stops_the_turn(llm_server, mcp_server):
    llm_server.script += [call_reply([("add", {"a": 1, "b": 1}), ("add", {"a": 2, "b": 2})])]
    agent, recorder = make_agent(llm_server, mcp_server)
    mcp_server.stop()
    result = agent.run("add")
    assert result.reason == "error" and "cannot reach" in recorder.errors[-1]
    assert [m["role"] for m in agent.messages[-2:]] == ["tool", "tool"]


def test_llm_error_is_reported(llm_server, mcp_server):
    llm_server.script.append(error_reply(500, "model exploded"))
    agent, recorder = make_agent(llm_server, mcp_server)
    assert agent.run("hi").reason == "error" and "model exploded" in recorder.errors[0]
