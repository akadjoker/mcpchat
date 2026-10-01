# mcpchat

Cliente de chat que liga um modelo de linguagem a **servidores MCP** ([Model Context Protocol](https://modelcontextprotocol.io)):
escreves o que queres, o modelo chama as *tools* dos servidores, vês cada chamada, o resultado e as imagens que
devolvem, e ele responde.

Programa nativo em C++17: janela da [zen_platform](https://github.com/akadjoker/zen_plataform) com OpenGL 3.3 e a
interface desenhada pelo [iGUI](https://github.com/akadjoker/iGUI). Um só executável, para Linux e Windows.

![mcpchat ligado ao CocoShape](docs/screenshot.png)

Não está preso a nenhum projeto: qualquer servidor MCP serve (o editor [CocoShape](https://github.com/akadjoker/cocoshape)
é um deles), e qualquer LLM com a API de chat da OpenAI (Ollama, LM Studio, vLLM, llama.cpp server, OpenAI, DeepSeek...).

```
tu ──► mcpchat ──(API OpenAI-compatível)──► LLM (local ou remoto)
          │
          └──(MCP: HTTP ou stdio)──► servidor(es) MCP ──► o programa (editor, ficheiros, ...)
```

- **MCP:** Streamable HTTP (respostas JSON ou *event-stream*, sessão `Mcp-Session-Id`) e stdio (processo filho);
  handshake `initialize` nas revisões `2024-11-05` a `2025-11-25`; vários servidores ao mesmo tempo.
- **LLM:** protocolo `chat/completions` com `tools`, em *streaming* ou não (se o servidor recusar *streaming* com
  tools, repete sem); as imagens das tools vão para o modelo quando ele as vê.
- **Janela:** o texto do modelo aparece à medida que chega, com blocos de código; cada chamada de tool fica numa caixa
  que abre com os argumentos, o resultado e as imagens; botão Copy em cada mensagem; Stop cancela o pedido em curso;
  as conversas podem ser gravadas em JSON.
- **Rede:** libcurl em Linux, WinHTTP em Windows. Em Windows não precisa de nenhuma DLL além das do sistema.

## Compilar

Precisa de CMake 3.16+, um compilador C++17 e git (a zen_platform e o iGUI são descarregados pelo CMake, fixados num
commit).

```bash
# Linux (Debian/Ubuntu)
sudo apt install libx11-dev libxrandr-dev libgl-dev libcurl4-openssl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/mcpchat

# Windows (Visual Studio)
cmake -S . -B build -A x64
cmake --build build --config Release

# Windows a partir de Linux (MinGW-w64, modelo de threads posix)
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j
```

`-DMCPCHAT_BUILD_GUI=OFF` compila só o núcleo e os testes (sem janela, sem zen_platform nem iGUI).

As versões prontas para Linux e Windows estão nas [releases](https://github.com/akadjoker/mcpchat/releases), feitas
pelo CI a cada tag `v*`.

## Usar

No primeiro arranque o mcpchat escreve uma configuração de exemplo em `~/.config/mcpchat/config.json` (Windows:
`%APPDATA%\mcpchat\config.json`) com o CocoShape e três perfis (Ollama, LM Studio, OpenAI). Tudo se muda em
**Settings**; o ficheiro também pode ser editado à mão.

- Escreve na caixa de baixo e **Ctrl+Enter** (ou Send) envia; Enter muda de linha.
- A barra de cima escolhe o perfil (o LLM) e mostra cada servidor: verde ligado, vermelho não (passa o rato por cima
  para ver as tools ou o erro). **Reconnect** volta a ligar aos servidores.
- **New chat** começa de novo; **Save chat** grava a conversa em `conversations/` ao lado da configuração.
- A roda do rato, a barra lateral e PageUp/PageDown percorrem a conversa; enquanto estás no fundo, ela acompanha o
  texto novo.

## Configuração

```json
{
  "current_profile": "ollama",
  "confirm": "destructive",
  "servers": [
    {"name": "cocoshape", "url": "http://127.0.0.1:7420/mcp", "token_env": "COCOSHAPE_API_TOKEN"},
    {"name": "files", "command": "npx", "args": ["-y", "@modelcontextprotocol/server-filesystem", "/home/eu/modelos"]}
  ],
  "profiles": [
    {"name": "ollama", "base_url": "http://localhost:11434/v1", "model": "qwen2.5:14b"},
    {"name": "openai", "base_url": "https://api.openai.com/v1", "model": "gpt-4.1",
     "api_key_env": "OPENAI_API_KEY", "vision": true}
  ]
}
```

**Servidores** (`servers`): `url` para HTTP ou `command` + `args` (+ `env`) para stdio; `headers` extra;
`token_env` = nome da variável de ambiente com um token *Bearer*; `enabled`; `timeout` (segundos).
Com um servidor, o modelo vê os nomes das tools tal como são; com vários, `servidor__tool`.

**Perfis** (`profiles`, um LLM cada):

| Campo | Significado |
|---|---|
| `base_url` | endereço OpenAI-compatível, com o prefixo de versão (`http://localhost:11434/v1`) |
| `model` | nome do modelo no servidor |
| `api_key_env` | **nome** da variável de ambiente com a chave (a chave nunca vai para o ficheiro) |
| `vision` | o modelo aceita imagens; é dito por ti, não adivinhado ("Images" em Settings) |
| `simplify_schema` | simplifica os esquemas das tools para servidores que rejeitam `oneOf`, `minItems`... |
| `stream` | resposta em *streaming* |
| `temperature`, `max_steps` (40), `request_timeout` (300), `context_chars` (120000) | |
| `system_prompt` | substitui o *system prompt* genérico; as `instructions` de cada servidor são sempre juntadas |

**Confirmação** (`confirm`): antes de correr uma tool, a janela pergunta (Allow / Deny) conforme a política:
`destructive` (as que o servidor marca com `destructiveHint: true`), `writes` (todas as que não são só de leitura) ou
`never`.

**Chaves e tokens** vêm da variável de ambiente indicada; em alternativa escrevem-se em Settings ("Set") e ficam só em
memória enquanto o programa corre, nunca no ficheiro.

## Com o CocoShape

```bash
cocoshape --api        # o editor, com MCP em http://127.0.0.1:7420/mcp
```

Com a configuração de exemplo o mcpchat liga-se logo a ele. O CocoShape manda as suas convenções (unidades, eixos,
como construir e verificar) nas `instructions` do MCP e o mcpchat junta-as ao *system prompt*. Para o modelo ver os
screenshots do editor, liga "Images" no perfil (só em modelos com visão). Modelos pequenos podem precisar de
"Simple schemas".

## Testes

```bash
ctest --test-dir build --output-on-failure        # servidores MCP e LLM falsos, sem rede
COCOSHAPE_BIN=/caminho/cocoshape xvfb-run -a ./build/tests/mcpchat_e2e   # contra o editor real
```

O CI corre os testes em Linux (também com AddressSanitizer e UBSan) e em Windows com MSVC, compila para Windows com
MinGW, corre o teste ponta a ponta contra o CocoShape e verifica que os executáveis Windows só importam DLLs do
sistema.

## Limites

- Do lado do LLM, só a API OpenAI-compatível (cobre Ollama, LM Studio, vLLM, OpenAI, DeepSeek...); outras APIs entram
  como mais um `LlmProvider` em `src/llm/`.
- Do MCP usa só *tools* (não *resources*, *prompts* nem *sampling*).
- O texto usa uma fonte (Roboto) com os caracteres latinos e símbolos comuns; caracteres fora desse conjunto (CJK,
  emoji) não aparecem. Do markdown, os blocos de código e os títulos têm aspeto próprio e o negrito perde os `**`; o
  resto aparece como texto.
- Em Windows a escala da interface é 1:1 com os píxeis do ecrã.
