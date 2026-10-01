"""The configuration file: MCP servers, LLM profiles and the confirmation policy, as JSON.

No secret is ever stored in it: servers and profiles name the environment variable that holds their token or key.
"""

import json
import os
import sys
from dataclasses import asdict, dataclass, field, fields
from pathlib import Path

FILE_VERSION = 1
CONFIRM_POLICIES = ("destructive", "writes", "never")


def config_dir():
    if sys.platform == "win32":
        base = Path(os.environ.get("APPDATA") or Path.home() / "AppData" / "Roaming")
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    return base / "mcpchat"


def default_config_path():
    return config_dir() / "config.json"


@dataclass
class ServerConfig:
    """An MCP server: `url` for Streamable HTTP, or `command` (+ `args`, `env`) for a stdio child process."""

    name: str
    url: str = ""
    command: str = ""
    args: list = field(default_factory=list)
    env: dict = field(default_factory=dict)
    headers: dict = field(default_factory=dict)
    token_env: str = ""            # environment variable with a bearer token for an HTTP server
    enabled: bool = True
    timeout: float = 300.0

    def problems(self):
        found = []
        if not self.name.strip():
            found.append("a server has no name")
        if bool(self.url) == bool(self.command):
            found.append(f"server '{self.name}': give either 'url' or 'command'")
        if self.url and not self.url.startswith(("http://", "https://")):
            found.append(f"server '{self.name}': 'url' must start with http:// or https://")
        return found


@dataclass
class Profile:
    """An LLM endpoint that speaks the OpenAI chat-completions protocol (Ollama, LM Studio, vLLM, OpenAI...)."""

    name: str
    base_url: str = ""
    model: str = ""
    api_key_env: str = ""          # environment variable with the key, never the key itself
    vision: bool = False           # the model accepts images; stated, not guessed
    simplify_schema: bool = False  # for servers that reject oneOf/minItems/... in tool schemas
    stream: bool = True
    temperature: float | None = None
    max_steps: int = 40
    request_timeout: float = 300.0
    context_chars: int = 120_000
    system_prompt: str = ""

    def problems(self):
        found = []
        if not self.name.strip():
            found.append("a profile has no name")
        if not self.base_url.startswith(("http://", "https://")):
            found.append(f"profile '{self.name}': 'base_url' must start with http:// or https://")
        if not self.model.strip():
            found.append(f"profile '{self.name}': 'model' is empty")
        if self.max_steps < 1:
            found.append(f"profile '{self.name}': 'max_steps' must be at least 1")
        return found


@dataclass
class Config:
    servers: list = field(default_factory=list)
    profiles: list = field(default_factory=list)
    current_profile: str = ""
    confirm: str = "destructive"

    def profile(self, name=None):
        wanted = name or self.current_profile
        for profile in self.profiles:
            if profile.name == wanted:
                return profile
        return self.profiles[0] if self.profiles and not name else None

    def problems(self):
        found = []
        for item in [*self.servers, *self.profiles]:
            found.extend(item.problems())
        if self.confirm not in CONFIRM_POLICIES:
            found.append(f"'confirm' must be one of {', '.join(CONFIRM_POLICIES)}")
        for kind, items in (("server", self.servers), ("profile", self.profiles)):
            names = [item.name for item in items]
            for name in sorted({n for n in names if names.count(n) > 1}):
                found.append(f"two {kind}s are called '{name}'")
        return found


def _build(cls, data, where):
    if not isinstance(data, dict):
        raise ValueError(f"{where}: expected an object")
    known = {f.name for f in fields(cls)}
    unknown = sorted(set(data) - known)
    if unknown:
        raise ValueError(f"{where}: unknown field(s) {', '.join(unknown)}")
    return cls(**data)


def load_config(path=None):
    path = Path(path) if path else default_config_path()
    if not path.exists():
        return Config()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except ValueError as error:
        raise ValueError(f"{path}: not valid JSON ({error})") from None
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected a JSON object")
    servers = [_build(ServerConfig, item, f"{path}: servers[{i}]") for i, item in enumerate(data.get("servers", []))]
    profiles = [_build(Profile, item, f"{path}: profiles[{i}]") for i, item in enumerate(data.get("profiles", []))]
    return Config(servers, profiles, data.get("current_profile", ""), data.get("confirm", "destructive"))


def save_config(config, path=None):
    path = Path(path) if path else default_config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    data = {"version": FILE_VERSION, "current_profile": config.current_profile, "confirm": config.confirm,
            "servers": [asdict(s) for s in config.servers], "profiles": [asdict(p) for p in config.profiles]}
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


EXAMPLE = {
    "current_profile": "ollama",
    "confirm": "destructive",
    "servers": [
        {"name": "cocoshape", "url": "http://127.0.0.1:7420/mcp", "token_env": "COCOSHAPE_API_TOKEN"},
    ],
    "profiles": [
        {"name": "ollama", "base_url": "http://localhost:11434/v1", "model": "qwen2.5:14b"},
        {"name": "lmstudio", "base_url": "http://localhost:1234/v1", "model": "local-model"},
        {"name": "openai", "base_url": "https://api.openai.com/v1", "model": "gpt-4.1",
         "api_key_env": "OPENAI_API_KEY", "vision": True},
    ],
}
