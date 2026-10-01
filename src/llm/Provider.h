#ifndef MCPCHAT_PROVIDER_H
#define MCPCHAT_PROVIDER_H

#include "util/Json.h"

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mcpchat
{

class CancelToken;

class LlmError : public std::runtime_error
{
public:
    explicit LlmError(const std::string& message, long httpStatus = 0)
        : std::runtime_error(message), httpStatus(httpStatus)
    {
    }
    long httpStatus;
};

// One function call the model asked for. `arguments` is empty when the model did not send a JSON object; `error`
// then says why.
struct ToolCall
{
    std::string id;
    std::string name;
    std::optional<Json> arguments;
    std::string rawArguments;
    std::string error;
};

struct Usage
{
    long long promptTokens = 0;
    long long completionTokens = 0;
};

struct AssistantMessage
{
    std::string content;
    std::vector<ToolCall> toolCalls;
    std::string finishReason;
    std::optional<Usage> usage;
};

using TextDelta = std::function<void(const std::string& text)>;

class LlmProvider
{
public:
    virtual ~LlmProvider() = default;
    // True when screenshots may be sent to the model.
    virtual bool supportsImages() const = 0;
    // `messages` are the conversation in this program's form (see Wire.h). Throws Cancelled when `cancel` fires,
    // LlmError on any other failure.
    virtual AssistantMessage complete(const std::vector<Json>& messages, const Json& tools, const TextDelta& onText,
                                      CancelToken* cancel) = 0;
};

} // namespace mcpchat

#endif
