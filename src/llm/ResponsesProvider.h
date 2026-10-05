#ifndef MCPCHAT_RESPONSES_PROVIDER_H
#define MCPCHAT_RESPONSES_PROVIDER_H

#include "llm/Provider.h"

#include <optional>

namespace mcpchat
{

// The conversation in this program's form -> a Responses API request body (without `model`, `stream` and friends).
// `tools` is expected in the chat-completions shape and comes out as flat function definitions. With `vision` off,
// images are left out. Free function so the mapping can be looked at on its own.
Json toResponsesRequest(const std::vector<Json>& messages, const Json& tools, bool vision);

// OpenAI's `/responses`: the endpoint the newer reasoning models need for function tools together with reasoning.
// `baseUrl` carries the version prefix, as for the other providers. The conversation is sent whole on every call
// (`store` is off), so nothing depends on the server remembering earlier turns.
class ResponsesProvider : public LlmProvider
{
public:
    struct Settings
    {
        std::string baseUrl;
        std::string model;
        std::string apiKey;
        bool vision = false;
        std::optional<double> temperature;
        // "low", "medium", "high"...; empty leaves it to the model.
        std::string reasoningEffort;
        bool stream = true;
        double timeoutSeconds = 300.0;
    };

    // Throws LlmError when the base URL is not an http(s) URL.
    explicit ResponsesProvider(Settings settings);

    bool supportsImages() const override
    {
        return mSettings.vision;
    }
    AssistantMessage complete(const std::vector<Json>& messages, const Json& tools, const TextDelta& onText,
                              CancelToken* cancel) override;

private:
    AssistantMessage exchange(const Json& payload, bool stream, const TextDelta& onText, CancelToken* cancel);

    Settings mSettings;
    std::string mEndpoint;
};

} // namespace mcpchat

#endif
