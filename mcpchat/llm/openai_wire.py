
import json

from .base import AssistantMessage, ToolCall, Usage


def to_wire_messages(messages, vision):
    """Conversation -> request `messages`.

    An image cannot ride in a `tool` message, so it follows the run of tool messages as a `user`
    data-URI message (a user message in between would break the tool_call/tool pairing).
    """
    wire = []
    pending_images = []

    def flush_images():
        if pending_images:
            plural = len(pending_images) > 1
            text = "Images returned by the tool calls above." if plural else "Image returned by the tool call above."
            parts = [{"type": "text", "text": text}]
            for image in pending_images:
                uri = f"data:{image.get('mimeType', 'image/png')};base64,{image['data']}"
                parts.append({"type": "image_url", "image_url": {"url": uri}})
            wire.append({"role": "user", "content": parts})
            pending_images.clear()

    for message in messages:
        role = message["role"]
        if role != "tool":
            flush_images()
        if role == "assistant":
            wire.append(_assistant_to_wire(message))
        elif role == "tool":
            wire.append({"role": "tool", "tool_call_id": message["tool_call_id"], "content": message["content"]})
            if vision and message.get("image"):
                pending_images.append(message["image"])
        else:
            wire.append({"role": role, "content": message["content"]})
    flush_images()
    return wire


def _assistant_to_wire(message):
    calls = message.get("tool_calls")
    wire = {"role": "assistant", "content": message.get("content") or ("" if not calls else None)}
    if calls:
        wire["tool_calls"] = calls
    return wire


def assistant_to_history(reply):
    """Arguments are stored as normalised JSON ("{}" when unusable): servers would reject the original text."""
    message = {"role": "assistant", "content": reply.content}
    if reply.tool_calls:
        message["tool_calls"] = [
            {"id": call.id, "type": "function",
             "function": {"name": call.name, "arguments": json.dumps(call.arguments or {})}}
            for call in reply.tool_calls
        ]
    return message


class ReplyBuilder:
    """Accumulates a reply from stream deltas; tool-call arguments arrive in fragments per `index`."""

    def __init__(self):
        self.text = []
        self.calls = {}  # index -> {"id", "name", "args": [str]}
        self.finish_reason = ""
        self.usage = None

    def add_chunk(self, chunk):
        if chunk.get("usage"):
            self._set_usage(chunk["usage"])
        choices = chunk.get("choices") or []
        if not choices:
            return ""
        choice = choices[0]
        self.finish_reason = choice.get("finish_reason") or self.finish_reason
        delta = choice.get("delta") or {}
        for fragment in delta.get("tool_calls") or []:
            self._add_call_fragment(fragment)
        text = delta.get("content") or ""
        if text:
            self.text.append(text)
        return text

    def add_message(self, response):
        if response.get("usage"):
            self._set_usage(response["usage"])
        choices = response.get("choices") or []
        if not choices:
            return ""
        choice = choices[0]
        self.finish_reason = choice.get("finish_reason") or ""
        message = choice.get("message") or {}
        for position, call in enumerate(message.get("tool_calls") or []):
            self._add_call_fragment({"index": position, **call})
        text = message.get("content") or ""
        if text:
            self.text.append(text)
        return text

    def build(self):
        calls, seen = [], set()
        for index in sorted(self.calls):
            entry = self.calls[index]
            call_id = entry["id"] or f"call_{index}"
            while call_id in seen:  # some servers reuse ids; tool results are matched by id
                call_id += "_"
            seen.add(call_id)
            calls.append(_parse_call(call_id, entry["name"], "".join(entry["args"])))
        return AssistantMessage("".join(self.text), calls, self.finish_reason, self.usage)

    def _add_call_fragment(self, fragment):
        index = fragment.get("index")
        if index is None:
            # No index: a fragment with an id starts a new call, one without continues the last.
            index = len(self.calls) if fragment.get("id") or not self.calls else max(self.calls)
        entry = self.calls.setdefault(index, {"id": "", "name": "", "args": []})
        entry["id"] = fragment.get("id") or entry["id"]
        function = fragment.get("function") or {}
        entry["name"] += function.get("name") or ""
        arguments = function.get("arguments")
        if arguments is not None:
            entry["args"].append(arguments if isinstance(arguments, str) else json.dumps(arguments))

    def _set_usage(self, usage):
        self.usage = Usage(usage.get("prompt_tokens") or 0, usage.get("completion_tokens") or 0)


def _parse_call(call_id, name, raw):
    if not name:
        return ToolCall(call_id, "", None, raw, "the tool call has no function name")
    if not raw.strip():
        return ToolCall(call_id, name, {}, raw)  # models send "" for commands without arguments
    try:
        arguments = json.loads(raw)
    except ValueError as error:
        return ToolCall(call_id, name, None, raw, f"the arguments are not valid JSON ({error})")
    if not isinstance(arguments, dict):
        return ToolCall(call_id, name, None, raw, "the arguments must be a JSON object")
    return ToolCall(call_id, name, arguments, raw)
