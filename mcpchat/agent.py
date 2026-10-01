"""The conversation loop: the model answers or asks for tools, the tools run on their MCP servers, the results go
back to the model, until it answers without asking for more."""

import base64
import json
from dataclasses import dataclass

from .history import prune_history, truncate
from .llm.base import Cancelled, CancelToken, LlmError
from .llm.openai_wire import assistant_to_history
from .mcp import McpError, McpTransportError
from .tools import function_tool

DEFAULT_SYSTEM_PROMPT = """\
You are an assistant that works through the tools of the connected MCP servers. Use them to do what the user asks,
check the results, and answer briefly in the user's language. If a tool returns an error, read it, fix the arguments
and try again instead of repeating the same call. Tool arguments must be a single JSON object.
"""


@dataclass
class AgentConfig:
    max_steps: int = 40
    keep_images: int = 2
    recent_results: int = 6
    old_result_chars: int = 600
    max_result_chars: int = 8000
    max_history_chars: int = 120_000
    simplify_schema: bool = False
    system_prompt: str = ""
    confirm: str = "destructive"  # destructive | writes | never


class AgentListener:
    def on_step(self, step, max_steps):
        pass

    def on_text_delta(self, text):
        pass

    def on_tool_call(self, call_id, name, arguments):
        pass

    def on_tool_result(self, call_id, name, text, is_error, images):
        """`images` is a list of (mime type, bytes)."""

    def on_error(self, message):
        pass


@dataclass
class RunResult:
    reason: str        # done | cancelled | max_steps | error
    steps: int = 0


@dataclass
class _Outcome:
    text: str
    is_error: bool = False
    images: list = None
    image: dict | None = None  # the image block sent to a model that can see
    fatal: bool = False  # the server is unreachable: no point in carrying on


class Agent:
    def __init__(self, provider, hub, listener=None, config=None, confirm=None):
        """`confirm(name, arguments) -> bool` is asked before a tool the confirm policy covers runs."""
        self.provider = provider
        self.hub = hub
        self.listener = listener or AgentListener()
        self.config = config or AgentConfig()
        self.confirm = confirm
        self.messages = []

    def reset(self):
        self.messages.clear()

    def system_prompt(self):
        prompt = (self.config.system_prompt or DEFAULT_SYSTEM_PROMPT).strip()
        for title, text in self.hub.instructions():
            prompt += f"\n\n# {title}\n{text}"
        return prompt + "\n"

    def tool_definitions(self):
        return [function_tool(name, exposed.tool, self.config.simplify_schema)
                for name, exposed in self.hub.tools.items()]

    def run(self, user_text, cancel=None):
        cancel = cancel or CancelToken()
        tools = self.tool_definitions()
        self.messages.append({"role": "user", "content": user_text})
        system = {"role": "system", "content": self.system_prompt()}

        for step in range(1, self.config.max_steps + 1):
            if cancel.is_set:
                return RunResult("cancelled", step - 1)
            self.listener.on_step(step, self.config.max_steps)
            self._prune()
            try:
                reply = self.provider.complete([system, *self.messages], tools, self.listener.on_text_delta, cancel)
            except Cancelled:
                return RunResult("cancelled", step)
            except LlmError as error:
                self.listener.on_error(str(error))
                return RunResult("error", step)

            self.messages.append(assistant_to_history(reply))
            if not reply.tool_calls:
                if not reply.content.strip():
                    self.listener.on_error("The model returned an empty answer.")
                return RunResult("done", step)
            outcome = self._run_tool_calls(reply.tool_calls, cancel)
            if outcome:
                return RunResult(outcome, step)

        self.listener.on_error(f"Stopped after {self.config.max_steps} steps without a final answer. "
                               "Send another message to let the model continue.")
        return RunResult("max_steps", self.config.max_steps)

    def _run_tool_calls(self, calls, cancel):
        for position, call in enumerate(calls):
            if cancel.is_set:
                # Every tool_call needs an answer or the next request would be invalid.
                for skipped in calls[position:]:
                    self._add_tool_message(skipped.id, _Outcome("Cancelled by the user before it ran."))
                return "cancelled"
            outcome = self._execute(call)
            self._add_tool_message(call.id, outcome)
            if outcome.fatal:
                for skipped in calls[position + 1:]:
                    self._add_tool_message(skipped.id, _Outcome("Not run: the server is unreachable."))
                return "error"
        return None

    def _execute(self, call):
        self.listener.on_tool_call(call.id, call.name, call.arguments if call.arguments is not None else call.raw_arguments)
        outcome = self._attempt(call)
        self.listener.on_tool_result(call.id, call.name, outcome.text, outcome.is_error, outcome.images or [])
        if outcome.fatal:
            self.listener.on_error(outcome.text)
        return outcome

    def _needs_confirmation(self, exposed):
        policy = self.config.confirm
        if policy == "never" or self.confirm is None:
            return False
        if policy == "writes":
            return not exposed.read_only
        return exposed.destructive

    def _attempt(self, call):
        if call.error:
            return _Outcome(f"ERROR: {call.error}. Send the arguments as one JSON object.", True)
        exposed = self.hub.tools.get(call.name)
        if exposed is None:
            names = ", ".join(sorted(self.hub.tools))
            return _Outcome(f"ERROR: there is no tool '{call.name}'. Available tools: {names}.", True)
        if self._needs_confirmation(exposed) and not self.confirm(call.name, call.arguments):
            return _Outcome(f"The user declined to run {call.name}. Do not retry it; continue without it or ask "
                            "the user.", True)
        try:
            result = self.hub.call(call.name, call.arguments)
        except McpTransportError as error:
            return _Outcome(f"ERROR: {error}", True, fatal=True)
        except McpError as error:
            return _Outcome(f"ERROR: {error.message}", True)

        text = result.text()
        if not text and result.structured is not None:
            text = json.dumps(result.structured, separators=(",", ":"))
        images = []
        for block in result.images():
            try:
                images.append((block.get("mimeType", "image/png"), base64.b64decode(block["data"], validate=True)))
            except ValueError:
                continue
        text = truncate(text or "(no text)", self.config.max_result_chars)
        outcome = _Outcome(text, result.is_error, images)
        if images:
            if self.provider.supports_images:
                outcome.image = result.images()[0]
            else:
                outcome.text += "\n(The tool returned an image, but you cannot see images.)"
        return outcome

    def _add_tool_message(self, call_id, outcome):
        message = {"role": "tool", "tool_call_id": call_id, "content": outcome.text}
        if outcome.image:
            message["image"] = {"mimeType": outcome.image.get("mimeType", "image/png"), "data": outcome.image["data"]}
        self.messages.append(message)

    def _prune(self):
        config = self.config
        prune_history(self.messages, keep_images=config.keep_images, recent_results=config.recent_results,
                      old_result_chars=config.old_result_chars, max_chars=config.max_history_chars)

    def export_conversation(self):
        messages = []
        for message in self.messages:
            message = dict(message)
            if "image" in message:
                message["image"] = {"mimeType": message["image"]["mimeType"], "data": "<omitted>"}
            messages.append(message)
        return {"system_prompt": self.system_prompt(), "messages": messages}
