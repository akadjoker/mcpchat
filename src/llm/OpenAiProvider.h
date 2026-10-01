#ifndef MCPCHAT_OPENAI_PROVIDER_H
#define MCPCHAT_OPENAI_PROVIDER_H

#include "llm/Provider.h"

#include <optional>

namespace mcpchat
{

// OpenAI-compatible `/chat/completions` with `tools`. `baseUrl` is used as given, so it carries any version prefix
// (`http://localhost:11434/v1`, but `https://api.deepseek.com`).
class OpenAiProvider : public LlmProvider
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
    };

    // Throws LlmError when the base URL is not an http(s) URL.
    explicit OpenAiProvider(Settings settings);

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
