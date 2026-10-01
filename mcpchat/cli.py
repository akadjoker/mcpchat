"""mcpchat on the console: chat with an LLM that works through MCP servers, and see every tool call it makes."""

import argparse
import getpass
import json
import os
import shlex
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import urlparse

from . import __version__
from .agent import Agent, AgentConfig, AgentListener
from .config import EXAMPLE, Config, Profile, ServerConfig, default_config_path, load_config, save_config
from .llm.base import CancelToken
from .llm.openai_compat import OpenAICompatProvider
from .secrets import SecretStore, llm_key_account
from .servers import ServerHub

HELP = """\
Commands:
  /tools            the tools the model can call
  /servers          the MCP servers and their state
  /prompt           the system prompt the model gets
  /reset            forget the conversation
  /save FILE        write the conversation as JSON
  /quit             leave (Ctrl+D works too)
Ctrl+C while the model works cancels the request."""


def _short(value, limit=160):
    text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, separators=(",", ":"))
    text = " ".join(text.split())
    return text if len(text) <= limit else text[:limit - 3] + "..."


class ConsoleListener(AgentListener):
    def __init__(self, out, image_dir, verbose=False):
        self.out = out
        self.image_dir = Path(image_dir)
        self.verbose = verbose
        self._in_text = False
        self._image_count = 0

    def _end_text(self):
        if self._in_text:
            self.out.write("\n")
            self._in_text = False

    def on_text_delta(self, text):
        if not self._in_text:
            self.out.write("\n")
            self._in_text = True
        self.out.write(text)
        self.out.flush()

    def on_tool_call(self, call_id, name, arguments):
        self._end_text()
        self.out.write(f"  → {name} {_short(arguments)}\n")
        self.out.flush()

    def on_tool_result(self, call_id, name, text, is_error, images):
        mark = "✗" if is_error else "←"
        body = text if self.verbose else _short(text)
        self.out.write(f"  {mark} {body}\n")
        for mime, data in images:
            self._image_count += 1
            extension = {"image/png": ".png", "image/jpeg": ".jpg", "image/webp": ".webp"}.get(mime, ".bin")
            self.image_dir.mkdir(parents=True, exist_ok=True)
            path = self.image_dir / f"{time.strftime('%H%M%S')}_{self._image_count:03d}_{name}{extension}"
            path.write_bytes(data)
            self.out.write(f"    [image: {path}]\n")
        self.out.flush()

    def on_error(self, message):
        self._end_text()
        self.out.write(f"  ! {message}\n")
        self.out.flush()

    def finish(self):
        self._end_text()


def _ad_hoc_server(spec):
    if spec.startswith(("http://", "https://")):
        name = (urlparse(spec).hostname or "server").replace(".", "_")
        return ServerConfig(name=name, url=spec)
    parts = shlex.split(spec)
    return ServerConfig(name=Path(parts[0]).stem or "server", command=parts[0], args=parts[1:])


def _build_config(args):
    config = load_config(args.config)
    if args.server:
        config.servers = [_ad_hoc_server(spec) for spec in args.server]
    if args.base_url or args.model:
        base = config.profile(args.profile) or Profile(name="command-line")
        profile = Profile(**{**base.__dict__, "name": "command-line"})
        profile.base_url = args.base_url or profile.base_url
        profile.model = args.model or profile.model
        if args.api_key_env:
            profile.api_key_env = args.api_key_env
        config.profiles = [profile, *[p for p in config.profiles if p.name != "command-line"]]
        config.current_profile = "command-line"
    elif args.profile:
        config.current_profile = args.profile
    if args.vision is not None:
        for profile in config.profiles:
            profile.vision = args.vision
    if args.yes:
        config.confirm = "never"
    return config


def _api_key(profile, secrets, interactive):
    resolved = secrets.resolve(llm_key_account(profile.name), profile.api_key_env)
    if resolved.value or not profile.api_key_env:
        return resolved.value
    if not interactive:
        return ""
    value = getpass.getpass(f"API key for '{profile.name}' (${profile.api_key_env} is not set): ")
    secrets.set_memory(llm_key_account(profile.name), value)
    return value


def _ask(out, name, arguments):
    out.write(f"  ? {name} {_short(arguments)}\n    run it? [y/N] ")
    out.flush()
    try:
        return input().strip().lower() in ("y", "yes", "s", "sim")
    except EOFError:
        return False


def _run_turn(agent, text, listener):
    cancel = CancelToken()
    result = {}
    worker = threading.Thread(target=lambda: result.update(outcome=agent.run(text, cancel)), daemon=True)
    worker.start()
    try:
        while worker.is_alive():
            worker.join(0.1)
    except KeyboardInterrupt:
        cancel.cancel()
        listener.on_error("cancelling...")
        worker.join()
    listener.finish()
    return result.get("outcome")


def _print_servers(hub, out):
    for server in hub.servers:
        if server.client:
            info = server.client.server_info
            label = info.get("title") or info.get("name") or server.name
            out.write(f"  ● {server.name}: {label} {info.get('version', '')} - {len(server.tools)} tools, "
                      f"MCP {server.client.protocol_version}\n")
        else:
            out.write(f"  ○ {server.name}: {server.error}\n")


def _print_tools(hub, out):
    for name, exposed in sorted(hub.tools.items()):
        flags = " (read-only)" if exposed.read_only else " (destructive)" if exposed.destructive else ""
        description = (exposed.tool.get("description") or "").split("\n")[0]
        out.write(f"  {name}{flags}: {_short(description, 100)}\n")


def main(argv=None, out=None):
    out = out or sys.stdout
    parser = argparse.ArgumentParser(prog="mcpchat", description=__doc__)
    parser.add_argument("--config", help=f"configuration file (default {default_config_path()})")
    parser.add_argument("--profile", help="LLM profile to use")
    parser.add_argument("--server", action="append", metavar="URL_OR_COMMAND",
                        help="an MCP server for this run instead of the configured ones: an http(s) URL, or a "
                             "command line for a stdio server (repeatable)")
    parser.add_argument("--base-url", help="OpenAI-compatible endpoint for this run, e.g. http://localhost:11434/v1")
    parser.add_argument("--model", help="model name for this run")
    parser.add_argument("--api-key-env", help="environment variable holding the API key for this run")
    parser.add_argument("--vision", action=argparse.BooleanOptionalAction, default=None,
                        help="the model accepts images (tool screenshots are sent to it)")
    parser.add_argument("--once", metavar="TEXT", help="send one message, print the answer and exit")
    parser.add_argument("--yes", action="store_true", help="run destructive tools without asking")
    parser.add_argument("--verbose", action="store_true", help="print tool results in full")
    parser.add_argument("--images", default=os.path.join(tempfile.gettempdir(), "mcpchat-images"),
                        help="where images returned by tools are saved")
    parser.add_argument("--init", action="store_true", help="write an example configuration file and exit")
    parser.add_argument("--version", action="version", version=f"mcpchat {__version__}")
    args = parser.parse_args(argv)

    if args.init:
        path = Path(args.config) if args.config else default_config_path()
        if path.exists():
            out.write(f"{path} already exists; not touching it.\n")
            return 1
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(EXAMPLE, indent=2) + "\n", encoding="utf-8")
        out.write(f"wrote {path}\n")
        return 0

    try:
        config = _build_config(args)
    except ValueError as error:
        out.write(f"mcpchat: {error}\n")
        return 2
    problems = config.problems()
    profile = config.profile(args.profile if not (args.base_url or args.model) else None)
    if profile is None:
        problems.append("no LLM profile: use --base-url and --model, or run 'mcpchat --init' and edit the file")
    if not config.servers:
        problems.append("no MCP server: use --server URL, or add one to the configuration file")
    if problems:
        for problem in problems:
            out.write(f"mcpchat: {problem}\n")
        return 2

    interactive = args.once is None and sys.stdin.isatty()
    secrets = SecretStore()
    hub = ServerHub(config.servers, secrets, "mcpchat", __version__).connect()
    out.write(f"mcpchat {__version__} - model {profile.model} at {profile.base_url}\n")
    _print_servers(hub, out)
    if not hub.tools:
        out.write("mcpchat: no tools available from any server.\n")
        hub.close()
        return 3

    provider = OpenAICompatProvider(profile.base_url, profile.model, _api_key(profile, secrets, interactive),
                                    vision=profile.vision, temperature=profile.temperature, stream=profile.stream,
                                    timeout=profile.request_timeout)
    agent_config = AgentConfig(max_steps=profile.max_steps, max_history_chars=profile.context_chars,
                               simplify_schema=profile.simplify_schema, system_prompt=profile.system_prompt,
                               confirm=config.confirm)
    listener = ConsoleListener(out, args.images, args.verbose)
    agent = Agent(provider, hub, listener, agent_config, confirm=lambda name, arguments: _ask(out, name, arguments))

    try:
        if args.once is not None:
            outcome = _run_turn(agent, args.once, listener)
            return 0 if outcome and outcome.reason == "done" else 1
        out.write("Type a message, or /help.\n")
        while True:
            try:
                text = input("\n> ").strip()
            except EOFError:
                out.write("\n")
                return 0
            except KeyboardInterrupt:
                out.write("\n")
                continue
            if not text:
                continue
            if text.startswith("/"):
                command, _, rest = text.partition(" ")
                if command in ("/quit", "/exit"):
                    return 0
                if command == "/help":
                    out.write(HELP + "\n")
                elif command == "/tools":
                    _print_tools(hub, out)
                elif command == "/servers":
                    _print_servers(hub, out)
                elif command == "/prompt":
                    out.write(agent.system_prompt())
                elif command == "/reset":
                    agent.reset()
                    out.write("  conversation cleared\n")
                elif command == "/save":
                    path = Path(rest.strip() or f"mcpchat-{time.strftime('%Y%m%d-%H%M%S')}.json")
                    path.write_text(json.dumps(agent.export_conversation(), indent=2, ensure_ascii=False) + "\n",
                                    encoding="utf-8")
                    out.write(f"  saved {path}\n")
                else:
                    out.write(f"  unknown command {command}; /help lists them\n")
                continue
            outcome = _run_turn(agent, text, listener)
            if outcome and outcome.reason == "max_steps":
                out.write("  (stopped at the step limit)\n")
    finally:
        hub.close()
