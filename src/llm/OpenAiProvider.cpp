#include "llm/OpenAiProvider.h"

#include "llm/Wire.h"
#include "net/Http.h"
#include "net/Sse.h"
#include "net/Url.h"
#include "util/Cancel.h"
#include "util/Strings.h"

namespace mcpchat
{

OpenAiProvider::OpenAiProvider(Settings settings) : mSettings(std::move(settings))
{
    net::Url url;
    if (!net::Url::parse(mSettings.baseUrl, url))
        throw LlmError("Invalid LLM base URL: '" + mSettings.baseUrl + "' (expected http://host:port/...)");
    std::string base = trim(mSettings.baseUrl);
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    mEndpoint = base + "/chat/completions";
}

std::string OpenAiProvider::errorMessage(long status, const std::string& body)
{
    std::string text = trim(body);
    const Json payload = Json::parse(text, nullptr, false);
    if (!payload.is_discarded())
    {
        const Json& error = payload.is_object() && payload.contains("error") ? payload["error"] : payload;
        if (error.is_object() && error.contains("message") && error["message"].is_string())
            text = error["message"].get<std::string>();
        else if (error.is_string())
            text = error.get<std::string>();
    }
    const std::string prefix = status ? "The LLM server answered HTTP " + std::to_string(status) + ": "
                                      : std::string("The LLM server reported an error: ");
    return prefix + std::string(utf8Prefix(text, 500));
}

AssistantMessage OpenAiProvider::complete(const std::vector<Json>& messages, const Json& tools, const TextDelta& onText,
                                          CancelToken* cancel)
{
    Json payload = {{"model", mSettings.model}, {"messages", toWireMessages(messages, mSettings.vision)}};
    if (tools.is_array() && !tools.empty())
        payload["tools"] = tools;
    if (mSettings.temperature)
        payload["temperature"] = *mSettings.temperature;
    try
    {
        return exchange(payload, mSettings.stream, onText, cancel);
    }
    catch (const LlmError& error)
    {
        // Some servers refuse `stream` together with `tools`: retry once without it.
        if (mSettings.stream && (error.httpStatus == 400 || error.httpStatus == 422) &&
            toLower(error.what()).find("stream") != std::string::npos)
        {
            mSettings.stream = false;
            return exchange(payload, false, onText, cancel);
        }
        throw;
    }
}

AssistantMessage OpenAiProvider::exchange(const Json& payload, bool stream, const TextDelta& onText,
                                          CancelToken* cancel)
{
    if (cancel && cancel->isSet())
        throw Cancelled();
    Json body = payload;
    body["stream"] = stream;

    net::Request request;
    request.method = "POST";
    request.url = mEndpoint;
    request.timeoutSeconds = mSettings.timeoutSeconds;
    request.body = dump(body);
    request.headers = {{"Content-Type", "application/json"},
                       {"Accept", stream ? "text/event-stream" : "application/json"},
                       {"User-Agent", "mcpchat"}};
    if (!mSettings.apiKey.empty())
        request.headers.push_back({"Authorization", "Bearer " + mSettings.apiKey});

    ReplyBuilder builder;
    std::string failure;
    std::string errorBody;
    bool streaming = false;
    net::SseParser parser(
        [&](const std::string& data)
        {
            if (trim(data) == "[DONE]")
                return false;
            const Json chunk = Json::parse(data, nullptr, false);
            if (chunk.is_discarded())
            {
                failure = "Invalid chunk in the LLM stream: " + std::string(utf8Prefix(data, 200));
                return false;
            }
            if (chunk.is_object() && chunk.contains("error") && !chunk["error"].is_null())
            {
                failure = errorMessage(0, data);
                return false;
            }
            const std::string text = builder.addChunk(chunk);
            if (!text.empty() && onText)
                onText(text);
            return true;
        });
    auto sink = [&](const net::Response& head, std::string_view chunk)
    {
        if (head.status >= 400)
        {
            if (errorBody.size() < 65536)
                errorBody.append(chunk.data(), chunk.size());
            return true;
        }
        streaming = head.header("Content-Type").find("text/event-stream") != std::string::npos;
        if (!streaming)
        {
            errorBody.append(chunk.data(), chunk.size());
            return true;
        }
        return parser.feed(chunk);
    };

    net::Response response;
    std::string error;
    if (!net::send(request, response, error, sink, cancel))
    {
        if (cancel && cancel->isSet())
            throw Cancelled();
        throw LlmError("Connection to the LLM failed: " + error);
    }
    if (cancel && cancel->isSet())
        throw Cancelled();
    if (response.status >= 400)
        throw LlmError(errorMessage(response.status, errorBody), response.status);
    if (streaming)
    {
        parser.finish();
        if (!failure.empty())
            throw LlmError(failure);
        return builder.build();
    }
    // Not a stream: either streaming was off or the server ignored the flag.
    const Json whole = Json::parse(errorBody, nullptr, false);
    if (whole.is_discarded())
        throw LlmError("The LLM answered with invalid JSON: " + std::string(utf8Prefix(errorBody, 200)));
    const std::string text = builder.addMessage(whole);
    if (!text.empty() && onText)
        onText(text);
    return builder.build();
}

} // namespace mcpchat
