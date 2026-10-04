#ifndef MCPCHAT_ANTHROPIC_PROVIDER_H
#define MCPCHAT_ANTHROPIC_PROVIDER_H

#include "llm/Provider.h"

#include <optional>

namespace mcpchat
{

// The conversation in this program's form -> a Messages API request body (without `model`, `stream` and friends).
// `tools` is expected in the chat-completions shape and comes out as Anthropic tool definitions. With `vision` off,
// images are left out. Free function so the mapping can be looked at on its own.
Json toAnthropicRequest(const std::vector<Json>& messages, const Json& tools, bool vision);

// Claude, whose API is not the OpenAI one: a system prompt of its own, content blocks, tool_use and tool_result,
// `x-api-key` and a mandatory `max_tokens`. `baseUrl` carries the version prefix, as for the OpenAI provider.
class AnthropicProvider : public LlmProvider
{
public:
    struct Settings
    {
        std::string baseUrl;
        std::string model;
        std::string apiKey;
        bool vision = false;
        std::optional<double> temperature;
        bool stream = true;
        double timeoutSeconds = 300.0;
        // Required by the API and not negotiable: the reply is cut at this many tokens.
        long long maxTokens = 8192;
    };

    // Throws LlmError when the base URL is not an http(s) URL.
    explicit AnthropicProvider(Settings settings);

    bool supportsImages() const override
    {
        return mSettings.vision;
    }
    AssistantMessage complete(const std::vector<Json>& messages, const Json& tools, const TextDelta& onText,
                              CancelToken* cancel) override;

    static std::string errorMessage(long status, const std::string& body);

private:
    AssistantMessage exchange(const Json& payload, bool stream, const TextDelta& onText, CancelToken* cancel);

    Settings mSettings;
    std::string mEndpoint;
};

} // namespace mcpchat

#endif
