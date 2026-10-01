"""stdio: the server is a child process; one JSON-RPC message per line on its stdin and stdout."""

import collections
import json
import queue
import subprocess
import threading

from .client import McpTransportError


class StdioTransport:
    def __init__(self, command, args=(), env=None, cwd=None, timeout=300.0):
        self.command = [command, *args]
        self.timeout = timeout
        self.stderr_tail = collections.deque(maxlen=50)
        self._pending = {}
        self._lock = threading.Lock()
        self._write_lock = threading.Lock()
        try:
            self._process = subprocess.Popen(self.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                             stderr=subprocess.PIPE, env=env, cwd=cwd, bufsize=0)
        except OSError as error:
            raise McpTransportError(f"cannot start {' '.join(self.command)}: {error}") from None
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def set_protocol_version(self, version):
        pass  # only the HTTP transport carries it

    def send(self, message):
        expects_reply = "id" in message and "method" in message
        waiter = None
        if expects_reply:
            waiter = queue.Queue(maxsize=1)
            with self._lock:
                self._pending[message["id"]] = waiter
        self._write(message)
        if not expects_reply:
            return None
        try:
            reply = waiter.get(timeout=self.timeout)
        except queue.Empty:
            raise McpTransportError(f"{self.command[0]} did not answer {message['method']} in {self.timeout:g} s") from None
        finally:
            with self._lock:
                self._pending.pop(message["id"], None)
        if reply is None:
            raise McpTransportError(self._exit_message())
        return reply

    def close(self):
        process = self._process
        if process.poll() is not None:
            return
        try:
            process.stdin.close()
            process.wait(timeout=3)
        except (OSError, subprocess.TimeoutExpired):
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()

    def _write(self, message):
        line = json.dumps(message, separators=(",", ":")) + "\n"
        with self._write_lock:
            try:
                self._process.stdin.write(line.encode("utf-8"))
                self._process.stdin.flush()
            except (OSError, ValueError):
                raise McpTransportError(self._exit_message()) from None

    def _read_stdout(self):
        for raw in self._process.stdout:
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except ValueError:
                self.stderr_tail.append(f"(not JSON on stdout) {line[:200]}")
                continue
            for item in message if isinstance(message, list) else [message]:
                self._dispatch(item)
        with self._lock:
            waiters = list(self._pending.values())
        for waiter in waiters:
            waiter.put(None)

    def _dispatch(self, message):
        if not isinstance(message, dict):
            return
        if "method" in message and "id" in message:
            # A request from the server: ping is answered, anything else this client does not offer.
            if message["method"] == "ping":
                self._write({"jsonrpc": "2.0", "id": message["id"], "result": {}})
            else:
                self._write({"jsonrpc": "2.0", "id": message["id"],
                             "error": {"code": -32601, "message": f"Method not found: {message['method']}"}})
            return
        with self._lock:
            waiter = self._pending.get(message.get("id"))
        if waiter is not None:
            waiter.put(message)

    def _read_stderr(self):
        for raw in self._process.stderr:
            self.stderr_tail.append(raw.decode("utf-8", "replace").rstrip())

    def _exit_message(self):
        code = self._process.poll()
        tail = "\n".join(list(self.stderr_tail)[-5:])
        state = f"exited with code {code}" if code is not None else "closed its output"
        return f"{self.command[0]} {state}" + (f":\n{tail}" if tail else "")
