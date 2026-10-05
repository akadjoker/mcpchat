# mcpchat

*[Português](README.pt.md) · English*

A small native chat client that connects a language model to **[MCP](https://modelcontextprotocol.io) servers**.
You type what you want, the model calls the servers' tools, you see every call, its result and any image it returns,
and the model answers.

Written in C++17: a [zen_platform](https://github.com/akadjoker/zen_plataform) window with OpenGL 3.3 and an
[iGUI](https://github.com/akadjoker/iGUI) interface. One executable for Linux and Windows — no Electron, no browser.

![mcpchat connected to CocoShape](docs/screenshot.png)

It is not tied to any project: any MCP server will do (the [CocoShape](https://github.com/akadjoker/cocoshape) 3D
editor is one, and a good demo: ask for "a cube that spins and rises over 2 seconds" and watch the animation appear).
It works with any LLM that speaks the OpenAI chat API (Ollama, LM Studio, vLLM, llama.cpp server, DeepSeek...), OpenAI's
Responses API (GPT-6 models), or the Anthropic Messages API (Claude).

```
you ──► mcpchat ──(chat/completions, Responses or Messages API)──► LLM (local or remote)
          │
          └──(MCP: HTTP or stdio)──► MCP server(s) ──► the program (editor, files, ...)
```

## Features

- **MCP:** Streamable HTTP (JSON or event-stream replies, `Mcp-Session-Id` session) and stdio (child process);
  `initialize` handshake for revisions `2024-11-05` to `2025-11-25`; several servers at once. Tools only
  (no resources, prompts or sampling).
- **LLM, three protocols chosen per profile:** `openai` (`chat/completions` with tools), `openai-responses`
  (OpenAI `/responses`, required by the GPT-6 models for tools with reasoning) and `anthropic` (Messages API,
  `tool_use`/`tool_result`). All stream or not; images returned by tools reach models that can see them, and you can
  attach your own images to a message.
- **Window:** text appears as it arrives, with code blocks; every tool call is a box that opens to show the
  arguments, the result and the images; a Copy button on each message; Stop cancels the request; conversations can be
  saved as JSON.
- **Safety:** before running a tool the window asks Allow / Deny according to a policy (`destructive`, `writes`,
  `never`). API keys come from environment variables and are never written to the config file.
- **Headless runner and tests:** `mcpchat_headless` drives the same agent without a window, so MCP servers can be
  tested with real models from CI. The test suite uses fake MCP and LLM servers, so it needs no network.
- **Network:** libcurl on Linux, WinHTTP on Windows (no extra DLLs on Windows).

## Build

Needs CMake 3.16+, a C++17 compiler and git (zen_platform and iGUI are fetched by CMake, pinned to a commit).

```bash
# Linux (Debian/Ubuntu): zenity or kdialog is only for the Attach button
sudo apt install libx11-dev libxrandr-dev libgl-dev libcurl4-openssl-dev zenity
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/mcpchat

# Windows (Visual Studio)
cmake -S . -B build -A x64
cmake --build build --config Release

# Windows from Linux (MinGW-w64, posix threads)
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j
```

`-DMCPCHAT_BUILD_GUI=OFF` builds only the core and the tests (no window, no zen_platform or iGUI).
Prebuilt Linux and Windows binaries are on the [releases page](https://github.com/akadjoker/mcpchat/releases), built by
CI on every `v*` tag.

## Quick start

1. Run `mcpchat` once: it writes `config.json` and `providers.json` to `~/.config/mcpchat/` (Windows:
   `%APPDATA%\mcpchat\`).
2. Export the key of your provider, e.g. `export OPENAI_API_KEY=...` **in the shell you launch mcpchat from**
   (or type it in Settings → "Set"; it then stays in memory only).
3. Pick a profile in the top bar, make sure your MCP server is green, and type. **Ctrl+Enter** sends.

With CocoShape: `cocoshape --api` serves MCP on `http://127.0.0.1:7420/mcp`, which the example config already uses.

Other things worth knowing: **Attach** (or `/attach <path>`) adds an image to the next message — only for profiles with
"Images" on; **New chat** / **Save chat**; the mouse wheel, the scrollbar and PageUp/PageDown scroll the conversation.

## Configuration

```json
{
  "current_profile": "openai",
  "confirm": "destructive",
  "servers": [
    {"name": "cocoshape", "url": "http://127.0.0.1:7420/mcp", "token_env": "COCOSHAPE_API_TOKEN"},
    {"name": "files", "command": "npx", "args": ["-y", "@modelcontextprotocol/server-filesystem", "/home/me/models"]}
  ],
  "profiles": [
    {"name": "ollama", "base_url": "http://localhost:11434/v1", "model": "qwen2.5:14b"},
    {"name": "openai", "api": "openai-responses", "base_url": "https://api.openai.com/v1", "model": "gpt-6-astra",
     "api_key_env": "OPENAI_API_KEY", "reasoning_effort": "medium", "vision": true},
    {"name": "claude", "api": "anthropic", "base_url": "https://api.anthropic.com/v1",
     "model": "claude-sonnet-5-5", "api_key_env": "ANTHROPIC_API_KEY", "vision": true}
  ]
}
```

**Servers:** `url` for HTTP or `command` + `args` (+ `env`) for stdio; extra `headers`; `token_env` = name of the
environment variable with a Bearer token; `enabled`; `timeout` (seconds). With one server the model sees tool names as
they are; with several, `server__tool`.

**Profiles** (one LLM each):

| Field | Meaning |
|---|---|
| `api` | `openai` (default) for `chat/completions` — Ollama, LM Studio, vLLM, DeepSeek —, `openai-responses` for OpenAI's `/responses`, or `anthropic` for Claude's Messages API |
| `base_url` | address with the version prefix (`http://localhost:11434/v1`, `https://api.anthropic.com/v1`) |
| `model` | model name on the server |
| `api_key_env` | the **name** of the environment variable holding the key (the key never goes in the file) |
| `vision` | the model accepts images; you state it, it is not guessed |
| `simplify_schema` | simplify tool schemas for servers that reject `oneOf`, `minItems`... |
| `stream` | streamed reply |
| `reasoning_effort` | `low`, `medium`, `high`... for `openai-responses`; empty leaves it to the model |
| `temperature`, `max_steps` (40), `request_timeout` (300), `context_chars` (120000) | |
| `system_prompt` | replaces the generic system prompt; each server's `instructions` are always added |

**Known providers:** in Settings → Model, the **Provider** combo fills in protocol, URL and key variable, and **Model**
offers that provider's models (any name can still be typed). The list lives in `providers.json` next to `config.json`,
because model names change often — add or fix them there.

## Tests

```bash
ctest --test-dir build --output-on-failure        # fake MCP and LLM servers, no network
COCOSHAPE_BIN=/path/to/cocoshape xvfb-run -a ./build/tests/mcpchat_e2e   # against the real editor
```

CI runs the tests on Linux (also with AddressSanitizer and UBSan) and on Windows with MSVC, cross-compiles with MinGW,
runs the end-to-end test against CocoShape and checks that the Windows executables import only system DLLs.

## Limits

- Claude's `max_tokens` is mandatory and fixed at 8192: a reply is cut there.
- Only MCP *tools* are used.
- The font (Roboto) covers Latin characters and common symbols; CJK and emoji do not render. Of markdown, code blocks
  and headings have their own look and bold loses its `**`; the rest shows as text.
- On Windows the UI scale is 1:1 with screen pixels.
- Dropping a file onto the window works only on Windows (the X11 backend has no XDND yet); on Linux use **Attach**.

## License

[MIT](LICENSE). Third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
