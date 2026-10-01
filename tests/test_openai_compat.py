import threading
import time

import pytest
from fake_llm_server import call_reply, error_reply, text_reply
from mcpchat.llm.base import Cancelled, CancelToken, LlmError
from mcpchat.llm.openai_compat import OpenAICompatProvider
from mcpchat.llm.openai_wire import to_wire_messages

USER = [{"role": "user", "content": "hi"}]


def provider_for(server, **options):
    return OpenAICompatProvider(server.base_url, "test-model", "key-123", **options)


def test_streams_text_in_deltas(llm_server):
    llm_server.script.append(text_reply("Hello from the model", fragments=4))
    deltas = []
    reply = provider_for(llm_server).complete(USER, [], deltas.append)
    assert len(deltas) > 1
    assert "".join(deltas) == reply.content == "Hello from the model"
    assert reply.tool_calls == []
    assert reply.finish_reason == "stop"


def test_request_shape_and_auth(llm_server):
    llm_server.script.append(text_reply("ok"))
    tools = [{"type": "function", "function": {"name": "t", "description": "", "parameters": {}}}]
    provider_for(llm_server, temperature=0.3).complete(USER, tools)
    request = llm_server.requests[0]
    assert request["path"] == "/v1/chat/completions"
    assert request["headers"]["Authorization"] == "Bearer key-123"
    body = request["body"]
    assert body["model"] == "test-model"
    assert body["stream"] is True
    assert body["temperature"] == 0.3
    assert body["tools"] == tools
    assert body["messages"] == [{"role": "user", "content": "hi"}]


def test_no_key_means_no_authorization_header(llm_server):
    llm_server.script.append(text_reply("ok"))
    OpenAICompatProvider(llm_server.base_url, "m").complete(USER, [])
    assert "Authorization" not in llm_server.requests[0]["headers"]
    assert "temperature" not in llm_server.requests[0]["body"]


def test_tool_call_arguments_arriving_in_fragments(llm_server):
    arguments = {"type": "sphere", "name": "dome", "scale": [1, 2, 3], "color": "#ff0000"}
    llm_server.script.append(call_reply([("add_primitive", arguments)], fragments=7))
    reply = provider_for(llm_server).complete(USER, [])
    (call,) = reply.tool_calls
    assert (call.id, call.name, call.arguments, call.error) == ("call_0", "add_primitive", arguments, "")
    assert reply.finish_reason == "tool_calls"


def test_several_tool_calls_and_text_in_one_reply(llm_server):
    llm_server.script.append(call_reply(
        [("add_primitive", {"type": "box"}), ("get_status", {})], text="Building.", fragments=3))
    reply = provider_for(llm_server).complete(USER, [])
    assert reply.content == "Building."
    assert [c.name for c in reply.tool_calls] == ["add_primitive", "get_status"]
    assert [c.id for c in reply.tool_calls] == ["call_0", "call_1"]


def test_empty_arguments_mean_no_arguments(llm_server):
    llm_server.script.append(call_reply([("get_status", "")]))
    (call,) = provider_for(llm_server).complete(USER, []).tool_calls
    assert call.arguments == {} and call.error == ""


@pytest.mark.parametrize("raw, fragment", [
    ('{"type": "box", ', "not valid JSON"),
    ("[1, 2]", "must be a JSON object"),
    ("type=box", "not valid JSON"),
])
def test_malformed_arguments_are_reported_not_raised(llm_server, raw, fragment):
    llm_server.script.append(call_reply([("add_primitive", raw)]))
    (call,) = provider_for(llm_server).complete(USER, []).tool_calls
    assert call.arguments is None
    assert fragment in call.error
    assert call.raw_arguments == raw


def test_non_streaming_request(llm_server):
    llm_server.script.append(call_reply([("get_status", {})], text="Checking"))
    deltas = []
    reply = provider_for(llm_server, stream=False).complete(USER, [], deltas.append)
    assert llm_server.requests[0]["body"]["stream"] is False
    assert deltas == ["Checking"]
    assert reply.tool_calls[0].name == "get_status"


def test_server_that_ignores_the_stream_flag(llm_server):
    llm_server.script.append(text_reply("whole", force_json=True))
    deltas = []
    reply = provider_for(llm_server).complete(USER, [], deltas.append)
    assert reply.content == "whole" and deltas == ["whole"]


def test_usage_is_reported_when_present(llm_server):
    llm_server.script.append(text_reply("x", usage={"prompt_tokens": 11, "completion_tokens": 3}))
    llm_server.script.append(text_reply("x", force_json=True, usage={"prompt_tokens": 5, "completion_tokens": 2}))
    streamed = provider_for(llm_server).complete(USER, [])
    whole = provider_for(llm_server).complete(USER, [])
    assert (streamed.usage.prompt_tokens, streamed.usage.completion_tokens) == (11, 3)
    assert (whole.usage.prompt_tokens, whole.usage.completion_tokens) == (5, 2)


def test_http_error_carries_status_and_server_message(llm_server):
    llm_server.script.append(error_reply(401, "Invalid API key"))
    with pytest.raises(LlmError) as caught:
        provider_for(llm_server).complete(USER, [])
    assert caught.value.http_status == 401
    assert "Invalid API key" in str(caught.value)


def test_refusing_stream_with_tools_falls_back_to_non_streaming(llm_server):
    llm_server.script.append(error_reply(400, "streaming is not supported with tools"))
    llm_server.script.append(call_reply([("get_status", {})]))
    llm_server.script.append(text_reply("second request, still non-streaming", force_json=True))
    provider = provider_for(llm_server)
    assert provider.complete(USER, []).tool_calls[0].name == "get_status"
    assert [r["body"]["stream"] for r in llm_server.requests] == [True, False]
    provider.complete(USER, [])
    assert llm_server.requests[2]["body"]["stream"] is False


def test_other_400_errors_are_not_retried(llm_server):
    llm_server.script.append(error_reply(400, "model not found"))
    with pytest.raises(LlmError):
        provider_for(llm_server).complete(USER, [])
    assert len(llm_server.requests) == 1


def test_unreachable_server_is_an_llm_error():
    with pytest.raises(LlmError) as caught:
        OpenAICompatProvider("http://127.0.0.1:1/v1", "m").complete(USER, [])
    assert "Cannot reach the LLM server" in str(caught.value)


def test_invalid_base_url_is_rejected():
    with pytest.raises(LlmError):
        OpenAICompatProvider("localhost:11434", "m")


def test_cancel_interrupts_a_stalled_stream(llm_server):
    llm_server.script.append(text_reply("partial", stall_after_first_chunk=True))
    cancel = CancelToken()
    seen = threading.Event()

    def on_delta(text):
        seen.set()

    outcome = {}

    def run():
        try:
            provider_for(llm_server).complete(USER, [], on_delta, cancel)
        except Cancelled:
            outcome["cancelled"] = time.monotonic()

    thread = threading.Thread(target=run)
    thread.start()
    assert seen.wait(5)
    pressed = time.monotonic()
    cancel.cancel()
    thread.join(5)
    assert not thread.is_alive()
    assert outcome["cancelled"] - pressed < 2


def test_already_cancelled_token_sends_nothing(llm_server):
    cancel = CancelToken()
    cancel.cancel()
    with pytest.raises(Cancelled):
        provider_for(llm_server).complete(USER, [], cancel=cancel)
    assert llm_server.requests == []


IMAGE = {"mimeType": "image/png", "data": "QUJD"}


def conversation_with_screenshot():
    return [
        {"role": "user", "content": "look"},
        {"role": "assistant", "content": "", "tool_calls": [
            {"id": "a", "type": "function", "function": {"name": "screenshot", "arguments": "{}"}},
            {"id": "b", "type": "function", "function": {"name": "get_status", "arguments": "{}"}}]},
        {"role": "tool", "tool_call_id": "a", "content": "{}", "image": IMAGE},
        {"role": "tool", "tool_call_id": "b", "content": "{}"},
    ]


def test_vision_profile_gets_the_image_as_a_user_message_after_all_tool_messages():
    wire = to_wire_messages(conversation_with_screenshot(), vision=True)
    assert [m["role"] for m in wire] == ["user", "assistant", "tool", "tool", "user"]
    assert "image" not in wire[2]
    parts = wire[4]["content"]
    assert parts[0]["type"] == "text" and parts[0]["text"].count("\n") == 0
    assert parts[1] == {"type": "image_url", "image_url": {"url": "data:image/png;base64,QUJD"}}
    assert wire[1]["content"] is None


def test_non_vision_profile_gets_text_only():
    wire = to_wire_messages(conversation_with_screenshot(), vision=False)
    assert [m["role"] for m in wire] == ["user", "assistant", "tool", "tool"]
    assert all("image_url" not in str(m) for m in wire)


def test_vision_request_reaches_the_server(llm_server):
    llm_server.script.append(text_reply("I see a box"))
    provider_for(llm_server, vision=True).complete(conversation_with_screenshot(), [])
    sent = llm_server.requests[0]["body"]["messages"]
    assert sent[-1]["content"][1]["image_url"]["url"].startswith("data:image/png;base64,")
    assert provider_for(llm_server, vision=True).supports_images
    assert not provider_for(llm_server).supports_images
