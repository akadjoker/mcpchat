# mcpchat

Cliente de chat que liga um modelo de linguagem a **servidores MCP** ([Model Context Protocol](https://modelcontextprotocol.io)):
escreves o que queres, o modelo chama as *tools* dos servidores, vês cada chamada e o resultado, e ele responde.

Não está preso a nenhum projeto: qualquer servidor MCP serve (o editor [CocoShape](https://github.com/akadjoker/CocoShape)
é um deles), e qualquer LLM com a API de chat da OpenAI (Ollama, LM Studio, vLLM, llama.cpp server, OpenAI, DeepSeek...).

```
você ──► mcpchat ──(API OpenAI-compatível)──► LLM (local ou remoto)
             │
             └──(MCP: HTTP ou stdio)──► servidor(es) MCP ──► o programa (editor, ficheiros, ...)
```

- **MCP:** Streamable HTTP (respostas JSON ou *event-stream*, sessão `Mcp-Session-Id`) e stdio (processo filho);
  handshake `initialize` nas revisões `2024-11-05` a `2025-11-25`; vários servidores ao mesmo tempo.
- **LLM:** protocolo `chat/completions` com `tools`, em *streaming* ou não; imagens das tools vão para o modelo
  quando ele as vê (`vision`).
- **Consola:** o texto do modelo aparece à medida que chega, cada chamada de tool e o resultado ficam visíveis, as
  imagens são gravadas em ficheiro, Ctrl+C cancela o pedido.
- Só a biblioteca padrão de Python (3.10+); `keyring` é opcional.

## Instalar

```bash
git clone https://github.com/akadjoker/mcpchat.git
cd mcpchat
pip install -e .            # ou corre sem instalar: python -m mcpchat
```

## Usar já, sem ficheiro de configuração

```bash
# um servidor MCP por HTTP e um modelo do Ollama
mcpchat --server http://127.0.0.1:7420/mcp --base-url http://localhost:11434/v1 --model qwen2.5:14b

# um servidor stdio (a linha de comando do servidor)
mcpchat --server "python3 /caminho/servidor.py" --base-url http://localhost:1234/v1 --model local-model

# uma pergunta só, para scripts
mcpchat --server http://127.0.0.1:7420/mcp --base-url http://localhost:11434/v1 --model qwen2.5:14b \
        --once "faz um barco com mastro"
```

Na consola: `/tools` (as tools que o modelo vê), `/servers`, `/prompt` (o *system prompt*), `/reset`, `/save FICHEIRO`,
`/quit`. Ctrl+C enquanto o modelo trabalha cancela o pedido.

```
> faz um barco com mastro
  → add_primitive {"type":"box","name":"hull","size":[2,0.5,0.8],"color":"#8b5a2b"}
  ← {"index":0,"name":"hull","triangles":12,...}
  → screenshot {"width":640,"height":480}
  ← {"height":480,"width":640,...}
    [image: /tmp/mcpchat-images/153012_001_screenshot.png]

Fiz um casco de 2 m com um mastro de 2,5 m no centro...
```

## Ficheiro de configuração

`mcpchat --init` escreve um exemplo em `~/.config/mcpchat/config.json` (Windows: `%APPDATA%\mcpchat`, macOS:
`~/Library/Application Support/mcpchat`); `--config FICHEIRO` usa outro.

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

**Perfis** (`profiles`, um LLM cada; `--profile NOME` escolhe):

| Campo | Significado |
|---|---|
| `base_url` | endereço OpenAI-compatível, com o prefixo de versão (`http://localhost:11434/v1`) |
| `model` | nome do modelo no servidor |
| `api_key_env` | **nome** da variável de ambiente com a chave (a chave nunca vai para o ficheiro) |
| `vision` | o modelo aceita imagens; é dito por ti, não adivinhado (`--vision` na linha de comando) |
| `simplify_schema` | simplifica os esquemas das tools para servidores que rejeitam `oneOf`, `minItems`... |
| `stream` | resposta em *streaming* (se o servidor recusar, repete sem) |
| `temperature`, `max_steps` (40), `request_timeout` (300), `context_chars` (120000) | |
| `system_prompt` | substitui o *system prompt* genérico; as `instructions` de cada servidor são sempre juntadas |

**Confirmação** (`confirm`): antes de correr uma tool, pergunta `[y/N]` conforme a política: `destructive` (as que o
servidor marca com `destructiveHint: true`), `writes` (todas as que não são só de leitura) ou `never`. `--yes` desliga.

As chaves vêm, por esta ordem, da variável de ambiente indicada, do porta-chaves do sistema (`pip install keyring`)
ou são pedidas na consola (só ficam em memória).

## Com o CocoShape

```bash
cocoshape --api                                   # o editor, com MCP em http://127.0.0.1:7420/mcp
mcpchat --server http://127.0.0.1:7420/mcp --base-url http://localhost:11434/v1 --model qwen2.5:14b --vision
```

O CocoShape manda as suas convenções (unidades, eixos, como construir e verificar) nas `instructions` do MCP; o
mcpchat junta-as ao *system prompt*. Modelos pequenos podem precisar de `simplify_schema`.

## Testes

```bash
pip install -e .[dev]
pytest                                            # servidores MCP e LLM falsos, sem rede
COCOSHAPE_BIN=/caminho/cocoshape pytest -m e2e    # contra o editor real (precisa de ecrã ou xvfb-run)
```

## Limites

- Só a API OpenAI-compatível do lado do LLM (cobre Ollama, LM Studio, vLLM, OpenAI, DeepSeek...); outras APIs entram
  como mais um *provider* em `mcpchat/llm/`.
- Do MCP usa só *tools* (não *resources*, *prompts* nem *sampling*).
- Servidores que só falam a revisão sem handshake (`2026-07-28`) ainda não.
- Interface só de consola; uma janela (Qt) pode vir por cima do mesmo agente.
