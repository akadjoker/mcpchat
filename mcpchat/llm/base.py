
import threading
from abc import ABC, abstractmethod
from contextlib import contextmanager
from dataclasses import dataclass, field


class LlmError(Exception):
    def __init__(self, message, http_status=None):
        super().__init__(message)
        self.http_status = http_status


class Cancelled(Exception):
    """The user pressed Stop."""


class CancelToken:
    """Thread-safe Stop flag that can also interrupt a blocking network read.

    Stop shuts the socket down from the GUI thread so a reader blocked between SSE lines wakes at once.
    """

    def __init__(self):
        self._event = threading.Event()
        self._lock = threading.Lock()
        self._callbacks = []

    @property
    def is_set(self):
        return self._event.is_set()

    def cancel(self):
        with self._lock:
            self._event.set()
            callbacks = list(self._callbacks)
        for callback in callbacks:
            callback()

    def wait(self, timeout):
        return self._event.wait(timeout)

    def raise_if_set(self):
        if self._event.is_set():
            raise Cancelled()

    @contextmanager
    def on_cancel(self, callback):
        with self._lock:
            already = self._event.is_set()
            if not already:
                self._callbacks.append(callback)
        if already:
            callback()
        try:
            yield
        finally:
            with self._lock:
                if callback in self._callbacks:
                    self._callbacks.remove(callback)


@dataclass
class ToolCall:
    """One function call requested by the model.

    `arguments` is None when the model did not send a JSON object; `error` then says why.
    """

    id: str
    name: str
    arguments: dict | None
    raw_arguments: str = ""
    error: str = ""


@dataclass
class Usage:
    prompt_tokens: int = 0
    completion_tokens: int = 0


@dataclass
class AssistantMessage:
    content: str = ""
    tool_calls: list = field(default_factory=list)
    finish_reason: str = ""
    usage: Usage | None = None


class LlmProvider(ABC):
    @property
    @abstractmethod
    def supports_images(self):
        """True when screenshots may be sent to the model."""

    @abstractmethod
    def complete(self, messages, tools, on_text_delta=None, cancel=None):
        """Returns the assistant's reply; raise Cancelled when `cancel` fires, LlmError on any other failure."""
