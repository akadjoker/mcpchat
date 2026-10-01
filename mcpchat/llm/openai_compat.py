"""Provider for OpenAI-compatible `/chat/completions` servers with `tools`.

`base_url` is used as given, so it must include any version prefix (`http://localhost:11434/v1`,
but `https://api.deepseek.com`). http.client is used so Stop can shut the socket down from another thread.
"""

import http.client
import json
import socket
import urllib.parse

from .base import Cancelled, CancelToken, LlmError, LlmProvider
from .openai_wire import ReplyBuilder, to_wire_messages

CONNECT_TIMEOUT = 10.0


class OpenAICompatProvider(LlmProvider):
    def __init__(self, base_url, model, api_key="", *, vision=False, temperature=None,
                 stream=True, timeout=300.0):
        parts = urllib.parse.urlsplit(base_url.strip())
        if parts.scheme not in ("http", "https") or not parts.hostname:
            raise LlmError(f"Invalid LLM base URL: {base_url!r} (expected http://host:port/...)")
        self._parts = parts
        self._path = parts.path.rstrip("/") + "/chat/completions"
        self.model = model
        self.api_key = api_key
        self.vision = vision
        self.temperature = temperature
        self.stream = stream
        self.timeout = timeout

    @property
    def supports_images(self):
        return self.vision

    def complete(self, messages, tools, on_text_delta=None, cancel=None):
        cancel = cancel or CancelToken()
        payload = {"model": self.model, "messages": to_wire_messages(messages, self.vision)}
        if tools:
            payload["tools"] = tools
        if self.temperature is not None:
            payload["temperature"] = self.temperature
        try:
            return self._exchange(payload, self.stream, on_text_delta, cancel)
        except LlmError as error:
            # Some servers refuse `stream` together with `tools`: retry once without it.
            if self.stream and error.http_status in (400, 422) and "stream" in str(error).lower():
                self.stream = False
                return self._exchange(payload, False, on_text_delta, cancel)
            raise

    def _exchange(self, payload, stream, on_text_delta, cancel):
        cancel.raise_if_set()
        payload = {**payload, "stream": stream}
        connection = self._connect()
        sock = connection.sock  # kept apart: the connection forgets it once a reply is 'close'
        try:
            with cancel.on_cancel(lambda: _shutdown(sock)):
                try:
                    return self._read_reply(connection, payload, stream, on_text_delta, cancel)
                except (OSError, http.client.HTTPException, ValueError) as error:
                    if cancel.is_set:
                        raise Cancelled() from error
                    raise LlmError(f"Connection to the LLM failed: {error}") from error
        finally:
            connection.close()

    def _connect(self):
        parts = self._parts
        cls = http.client.HTTPSConnection if parts.scheme == "https" else http.client.HTTPConnection
        connection = cls(parts.hostname, parts.port, timeout=CONNECT_TIMEOUT)
        try:
            connection.connect()
        except OSError as error:
            raise LlmError(f"Cannot reach the LLM server at {parts.netloc}: {error}") from error
        connection.sock.settimeout(self.timeout)
        return connection

    def _read_reply(self, connection, payload, stream, on_text_delta, cancel):
        headers = {
            "Content-Type": "application/json",
            "Accept": "text/event-stream" if stream else "application/json",
            "User-Agent": "mcpchat",
        }
        if self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"
        connection.request("POST", self._path, json.dumps(payload).encode(), headers)
        response = connection.getresponse()
        if response.status >= 400:
            raise LlmError(_error_message(response.status, response.read(65536)), response.status)

        builder = ReplyBuilder()
        if "text/event-stream" in (response.getheader("Content-Type") or ""):
            self._read_stream(response, builder, on_text_delta, cancel)
        else:
            # Not a stream: either streaming was off or the server ignored the flag.
            body = response.read()
            try:
                text = builder.add_message(json.loads(body))
            except ValueError as error:
                raise LlmError(f"The LLM answered with invalid JSON: {body[:200]!r}") from error
            if text and on_text_delta:
                on_text_delta(text)
        cancel.raise_if_set()
        return builder.build()

    @staticmethod
    def _read_stream(response, builder, on_text_delta, cancel):
        for raw in response:
            cancel.raise_if_set()
            line = raw.decode("utf-8", "replace").strip()
            if not line.startswith("data:"):
                continue  # blank separators, ": keep-alive" comments, "event:" names
            data = line[5:].strip()
            if data == "[DONE]":
                return
            try:
                chunk = json.loads(data)
            except ValueError as error:
                raise LlmError(f"Invalid chunk in the LLM stream: {data[:200]!r}") from error
            if isinstance(chunk, dict) and chunk.get("error"):
                raise LlmError(_error_message(None, json.dumps(chunk).encode()))
            text = builder.add_chunk(chunk)
            if text and on_text_delta:
                on_text_delta(text)


def _shutdown(sock):
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass


def _error_message(status, body):
    text = body.decode("utf-8", "replace").strip()
    try:
        payload = json.loads(text)
        error = payload.get("error", payload) if isinstance(payload, dict) else payload
        text = error.get("message", text) if isinstance(error, dict) else str(error)
    except ValueError:
        pass
    prefix = f"The LLM server answered HTTP {status}: " if status else "The LLM server reported an error: "
    return prefix + text[:500]
