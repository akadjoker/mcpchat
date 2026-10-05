#include "llm/ResponsesProvider.h"

#include "llm/OpenAiProvider.h"
#include "llm/Wire.h"
#include "net/Http.h"
#include "net/Sse.h"
#include "net/Url.h"
#include "util/Cancel.h"
#include "util/Strings.h"

namespace mcpchat
{

namespace
{
Json inputText(const std::string& text)
{
    return {{"type", "input_text"}, {"text", text}};
}

Json inputImage(const std::string& uri)
{
    return {{"type", "input_image"}, {"image_url", uri}};
}

Json userParts(const Json& content, bool vision)
{
    if (!content.is_array())
        return Json::array({inputText(content.is_string() ? content.get<std::string>() : std::string())});
    Json parts = Json::array();
    for (const Json& part : content)
    {
        const std::string type = part.value("type", "");
        if (type == "text")
            parts.push_back(inputText(part.value("text", std::string())));
        else if (type == "image_url" && vision && part.contains("image_url"))
            parts.push_back(inputImage(part["image_url"].value("url", std::string())));
    }
    if (parts.empty())
        parts.push_back(inputText(std::string()));
    return parts;
}

Json responsesTools(const Json& tools)
{
    if (!tools.is_array())
        return Json::array();
    Json out = Json::array();
    for (const Json& tool : tools)
    {
        const Json function = tool.value("function", Json::object());
        Json item = {{"type", "function"}, {"name", function.value("name", std::string())}};
        const std::string description = function.value("description", std::string());
        if (!description.empty())
            item["description"] = description;
        Json schema = function.value("parameters", Json::object());
        if (!schema.is_object() || schema.empty())
            schema = {{"type", "object"}, {"properties", Json::object()}};
        item["parameters"] = std::move(schema);
        out.push_back(std::move(item));
    }
    return out;
}

// A reply built from the stream events, or from the `output` of a whole response.
class ReplyCollector
{
public:
    // The text the event added, empty for the events that add none.
    std::string add(const Json& event)
    {
        const std::string type = event.value("type", "");
        if (type == "response.output_text.delta" && event.value("delta", Json()).is_string())
        {
            std::string text = event["delta"].get<std::string>();
            mText += text;
            return text;
        }
        if (type == "response.output_item.done")
            addItem(event.value("item", Json::object()), false);
        else if (type == "response.completed" || type == "response.incomplete")
            addResponse(event.value("response", Json::object()), false);
        return {};
    }

    // The text the response added that no delta had carried.
    std::string addResponse(const Json& response, bool takeItems)
    {
        std::string text;
        if (takeItems && response.contains("output") && response["output"].is_array())
            for (const Json& item : response["output"])
                text += addItem(item, true);
        if (response.contains("usage") && response["usage"].is_object())
        {
            Usage usage;
            usage.promptTokens = response["usage"].value("input_tokens", 0LL);
            usage.completionTokens = response["usage"].value("output_tokens", 0LL);
            mUsage = usage;
        }
        if (response.value("status", "") == "incomplete")
            mIncomplete = true;
        return text;
    }

    AssistantMessage build() const
    {
        AssistantMessage reply;
        reply.content = mText;
        reply.toolCalls = mCalls;
        reply.usage = mUsage;
        reply.finishReason = !mCalls.empty() ? "tool_calls" : (mIncomplete ? "length" : "stop");
        return reply;
    }

private:
    std::string addItem(const Json& item, bool takeText)
    {
        const std::string type = item.value("type", "");
        if (type == "function_call")
            mCalls.push_back(parseToolCall(item.value("call_id", std::string()), item.value("name", std::string()),
                                           item.value("arguments", std::string())));
        else if (type == "message" && takeText && item.contains("content") && item["content"].is_array())
        {
            std::string text;
            for (const Json& part : item["content"])
                if (part.value("type", "") == "output_text")
                    text += part.value("text", std::string());
            mText += text;
            return text;
        }
        return {};
    }

    std::string mText;
    std::vector<ToolCall> mCalls;
    std::optional<Usage> mUsage;
    bool mIncomplete = false;
};
} // namespace

Json toResponsesRequest(const std::vector<Json>& messages, const Json& tools, bool vision)
{
    Json out = Json::object();
    Json input = Json::array();
    std::size_t i = 0;
    while (i < messages.size())
    {
        const Json& message = messages[i];
        const std::string role = message.value("role", "");
        if (role == "system")
        {
            out["instructions"] = message.value("content", std::string());
            ++i;
            continue;
        }
        if (role == "tool")
        {
            // An image cannot ride in a function output, so it follows the run of results as a user message.
            Json images = Json::array();
            while (i < messages.size() && messages[i].value("role", "") == "tool")
            {
                const Json& result = messages[i];
                input.push_back({{"type", "function_call_output"},
                                 {"call_id", result.value("tool_call_id", std::string())},
                                 {"output", result.value("content", std::string())}});
                if (vision && result.contains("image") && result["image"].is_object())
                    images.push_back(
                        inputImage("data:" + result["image"].value("mimeType", std::string("image/png")) +
                                   ";base64," + result["image"].value("data", std::string())));
                ++i;
            }
            if (!images.empty())
                input.push_back({{"role", "user"}, {"content", std::move(images)}});
            continue;
        }
        if (role == "assistant")
        {
            const std::string text = message.contains("content") && message["content"].is_string()
                                         ? message["content"].get<std::string>()
                                         : std::string();
            if (!text.empty())
                input.push_back({{"role", "assistant"}, {"content", text}});
            if (message.contains("tool_calls") && message["tool_calls"].is_array())
                for (const Json& call : message["tool_calls"])
                {
                    const Json function = call.value("function", Json::object());
                    input.push_back({{"type", "function_call"},
                                     {"call_id", call.value("id", std::string())},
                                     {"name", function.value("name", std::string())},
                                     {"arguments", function.value("arguments", std::string("{}"))}});
                }
            ++i;
            continue;
        }
        input.push_back({{"role", "user"}, {"content", userParts(message.value("content", Json()), vision)}});
        ++i;
    }
    out["input"] = std::move(input);
    const Json converted = responsesTools(tools);
    if (!converted.empty())
        out["tools"] = converted;
    return out;
}

ResponsesProvider::ResponsesProvider(Settings settings) : mSettings(std::move(settings))
{
    net::Url url;
    if (!net::Url::parse(mSettings.baseUrl, url))
        throw LlmError("Invalid LLM base URL: '" + mSettings.baseUrl + "' (expected http://host:port/...)");
    std::string base = trim(mSettings.baseUrl);
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    mEndpoint = base + "/responses";
}

AssistantMessage ResponsesProvider::complete(const std::vector<Json>& messages, const Json& tools,
                                             const TextDelta& onText, CancelToken* cancel)
{
    Json payload = toResponsesRequest(messages, tools, mSettings.vision);
    payload["model"] = mSettings.model;
    payload["store"] = false;
    if (mSettings.temperature)
        payload["temperature"] = *mSettings.temperature;
    if (!mSettings.reasoningEffort.empty())
        payload["reasoning"] = {{"effort", mSettings.reasoningEffort}};
    return exchange(payload, mSettings.stream, onText, cancel);
}

AssistantMessage ResponsesProvider::exchange(const Json& payload, bool stream, const TextDelta& onText,
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

    ReplyCollector collector;
    std::string failure;
    std::string errorBody;
    bool streaming = false;
    net::SseParser parser(
        [&](const std::string& data)
        {
            if (trim(data) == "[DONE]")
                return false;
            const Json event = Json::parse(data, nullptr, false);
            if (event.is_discarded())
            {
                failure = "Invalid event in the LLM stream: " + std::string(utf8Prefix(data, 200));
                return false;
            }
            const std::string type = event.value("type", "");
            if (type == "error")
            {
                failure = OpenAiProvider::errorMessage(0, dump(event));
                return false;
            }
            if (type == "response.failed")
            {
                const Json response = event.value("response", Json::object());
                failure = OpenAiProvider::errorMessage(0, dump(response));
                return false;
            }
            const std::string text = collector.add(event);
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
        throw LlmError(OpenAiProvider::errorMessage(response.status, errorBody), response.status);
    if (streaming)
    {
        parser.finish();
        if (!failure.empty())
            throw LlmError(failure);
        return collector.build();
    }
    // Not a stream: either streaming was off or the server ignored the flag.
    const Json whole = Json::parse(errorBody, nullptr, false);
    if (whole.is_discarded())
        throw LlmError("The LLM answered with invalid JSON: " + std::string(utf8Prefix(errorBody, 200)));
    if (whole.is_object() && whole.contains("error") && !whole["error"].is_null())
        throw LlmError(OpenAiProvider::errorMessage(0, errorBody));
    const std::string text = collector.addResponse(whole, true);
    if (!text.empty() && onText)
        onText(text);
    return collector.build();
}

} // namespace mcpchat
