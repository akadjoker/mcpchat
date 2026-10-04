#include "llm/AnthropicProvider.h"

#include "llm/Wire.h"
#include "net/Http.h"
#include "net/Sse.h"
#include "net/Url.h"
#include "util/Cancel.h"
#include "util/Strings.h"

#include <map>

namespace mcpchat
{

const char* const kAnthropicVersion = "2023-06-01";

namespace
{
Json textBlock(const std::string& text)
{
    return {{"type", "text"}, {"text", text}};
}

Json imageBlock(const std::string& mimeType, const std::string& data)
{
    return {{"type", "image"},
            {"source", {{"type", "base64"}, {"media_type", mimeType}, {"data", data}}}};
}

// "data:image/png;base64,AAAA" -> the image block it stands for. Anything else than a data URI is dropped by the
// caller.
Json imageBlockFromUri(const std::string& uri)
{
    const std::size_t comma = uri.find(',');
    if (comma == std::string::npos)
        return Json();
    const std::string header = uri.substr(0, comma);
    const std::size_t colon = header.find(':');
    const std::size_t semicolon = header.find(';');
    std::string mimeType = "image/png";
    if (colon != std::string::npos && semicolon != std::string::npos && semicolon > colon + 1)
        mimeType = header.substr(colon + 1, semicolon - colon - 1);
    return imageBlock(mimeType, uri.substr(comma + 1));
}

// A tool message becomes a tool_result block; a screenshot the tool returned rides inside it.
Json toolResultBlock(const Json& message, bool vision)
{
    Json content = Json::array({textBlock(message.value("content", std::string()))});
    if (vision && message.contains("image") && message["image"].is_object())
        content.push_back(imageBlock(message["image"].value("mimeType", std::string("image/png")),
                                     message["image"].value("data", std::string())));
    return {{"type", "tool_result"}, {"tool_use_id", message.value("tool_call_id", "")}, {"content", content}};
}

// A user message is either plain text or the parts of one with images attached.
Json userContent(const Json& content, bool vision)
{
    if (!content.is_array())
        return Json::array({textBlock(content.is_string() ? content.get<std::string>() : std::string())});
    Json blocks = Json::array();
    for (const Json& part : content)
    {
        const std::string type = part.value("type", "");
        if (type == "text")
            blocks.push_back(textBlock(part.value("text", std::string())));
        else if (type == "image_url" && vision && part.contains("image_url"))
        {
            Json block = imageBlockFromUri(part["image_url"].value("url", std::string()));
            if (!block.is_null())
                blocks.push_back(std::move(block));
        }
    }
    if (blocks.empty())
        blocks.push_back(textBlock(std::string()));
    return blocks;
}

Json anthropicTools(const Json& tools)
{
    if (!tools.is_array())
        return Json::array();
    Json out = Json::array();
    for (const Json& tool : tools)
    {
        const Json function = tool.value("function", Json::object());
        Json item = {{"name", function.value("name", std::string())}};
        const std::string description = function.value("description", std::string());
        if (!description.empty())
            item["description"] = description;
        Json schema = function.value("parameters", Json::object());
        if (!schema.is_object() || schema.empty())
            schema = {{"type", "object"}, {"properties", Json::object()}};
        item["input_schema"] = std::move(schema);
        out.push_back(std::move(item));
    }
    return out;
}

// A reply built from the stream events; `content_block_start` opens a block and the deltas fill it in.
class StreamBuilder
{
public:
    // The text the event added, empty for the events that add none.
    std::string add(const Json& event)
    {
        const std::string type = event.value("type", "");
        if (type == "content_block_start")
        {
            const int index = event.value("index", 0);
            const Json block = event.value("content_block", Json::object());
            if (block.value("type", "") == "tool_use")
            {
                mCalls[index].id = block.value("id", std::string());
                mCalls[index].name = block.value("name", std::string());
            }
            return {};
        }
        if (type == "content_block_delta")
        {
            const int index = event.value("index", 0);
            const Json delta = event.value("delta", Json::object());
            const std::string kind = delta.value("type", "");
            if (kind == "text_delta")
            {
                std::string text = delta.value("text", std::string());
                mText += text;
                return text;
            }
            if (kind == "input_json_delta")
                mCalls[index].arguments += delta.value("partial_json", std::string());
            return {};
        }
        if (type == "message_start")
        {
            const Json message = event.value("message", Json::object());
            if (message.contains("usage") && message["usage"].is_object())
            {
                if (!mUsage)
                    mUsage.emplace();
                mUsage->promptTokens = message["usage"].value("input_tokens", 0LL);
            }
            return {};
        }
        if (type == "message_delta")
        {
            const Json delta = event.value("delta", Json::object());
            if (delta.contains("stop_reason") && delta["stop_reason"].is_string())
                mFinishReason = delta["stop_reason"].get<std::string>();
            if (event.contains("usage") && event["usage"].is_object())
            {
                if (!mUsage)
                    mUsage.emplace();
                mUsage->completionTokens = event["usage"].value("output_tokens", 0LL);
            }
            return {};
        }
        return {};
    }

    AssistantMessage build() const
    {
        AssistantMessage reply;
        reply.content = mText;
        reply.finishReason = mFinishReason;
        if (mUsage)
            reply.usage = *mUsage;
        for (const auto& entry : mCalls)
        {
            ToolCall call;
            call.id = entry.second.id;
            call.name = entry.second.name;
            call.rawArguments = entry.second.arguments;
            call.arguments = Json::parse(entry.second.arguments, nullptr, false);
            if (call.arguments->is_discarded())
            {
                call.arguments.reset();
                call.error = "the arguments are not valid JSON";
            }
            reply.toolCalls.push_back(std::move(call));
        }
        return reply;
    }

private:
    struct PartialCall
    {
        std::string id;
        std::string name;
        std::string arguments;
    };

    std::string mText;
    std::map<int, PartialCall> mCalls;
    std::string mFinishReason;
    std::optional<Usage> mUsage;
};

AssistantMessage fromWholeMessage(const Json& body)
{
    AssistantMessage reply;
    if (body.contains("content") && body["content"].is_array())
    {
        for (const Json& block : body["content"])
        {
            const std::string type = block.value("type", "");
            if (type == "text")
                reply.content += block.value("text", std::string());
            else if (type == "tool_use")
            {
                ToolCall call;
                call.id = block.value("id", std::string());
                call.name = block.value("name", std::string());
                const Json input = block.value("input", Json::object());
                call.rawArguments = dump(input);
                call.arguments = input;
                reply.toolCalls.push_back(std::move(call));
            }
        }
    }
    if (body.contains("stop_reason") && body["stop_reason"].is_string())
        reply.finishReason = body["stop_reason"].get<std::string>();
    if (body.contains("usage") && body["usage"].is_object())
    {
        Usage usage;
        usage.promptTokens = body["usage"].value("input_tokens", 0LL);
        usage.completionTokens = body["usage"].value("output_tokens", 0LL);
        reply.usage = usage;
    }
    return reply;
}
} // namespace

Json toAnthropicRequest(const std::vector<Json>& messages, const Json& tools, bool vision)
{
    Json out = Json::object();
    Json conversation = Json::array();
    std::size_t i = 0;
    while (i < messages.size())
    {
        const Json& message = messages[i];
        const std::string role = message.value("role", "");
        if (role == "system")
        {
            out["system"] = message.value("content", std::string());
            ++i;
            continue;
        }
        if (role == "tool")
        {
            // Every result of one assistant turn goes into a single user message.
            Json blocks = Json::array();
            while (i < messages.size() && messages[i].value("role", "") == "tool")
            {
                blocks.push_back(toolResultBlock(messages[i], vision));
                ++i;
            }
            conversation.push_back({{"role", "user"}, {"content", std::move(blocks)}});
            continue;
        }
        if (role == "assistant")
        {
            Json blocks = Json::array();
            const std::string text = message.contains("content") && message["content"].is_string()
                                         ? message["content"].get<std::string>()
                                         : std::string();
            if (!text.empty())
                blocks.push_back(textBlock(text));
            if (message.contains("tool_calls") && message["tool_calls"].is_array())
            {
                for (const Json& call : message["tool_calls"])
                {
                    const Json function = call.value("function", Json::object());
                    Json input = Json::object();
                    const Json parsed = Json::parse(function.value("arguments", std::string()), nullptr, false);
                    if (parsed.is_object())
                        input = parsed;
                    blocks.push_back({{"type", "tool_use"},
                                      {"id", call.value("id", std::string())},
                                      {"name", function.value("name", std::string())},
                                      {"input", std::move(input)}});
                }
            }
            // An empty text block is an error there; a turn with nothing in it is left out altogether.
            if (!blocks.empty())
                conversation.push_back({{"role", "assistant"}, {"content", std::move(blocks)}});
            ++i;
            continue;
        }
        conversation.push_back(
            {{"role", "user"}, {"content", userContent(message.value("content", Json()), vision)}});
        ++i;
    }
    out["messages"] = std::move(conversation);
    const Json converted = anthropicTools(tools);
    if (!converted.empty())
        out["tools"] = converted;
    return out;
}

AnthropicProvider::AnthropicProvider(Settings settings) : mSettings(std::move(settings))
{
    net::Url url;
    if (!net::Url::parse(mSettings.baseUrl, url))
        throw LlmError("Invalid LLM base URL: '" + mSettings.baseUrl + "' (expected http://host:port/...)");
    std::string base = trim(mSettings.baseUrl);
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    // The version prefix is part of the base URL, as for the OpenAI provider, but a bare host is accepted too.
    mEndpoint = base + (endsWith(base, "/v1") ? "/messages" : "/v1/messages");
}

std::string AnthropicProvider::errorMessage(long status, const std::string& body)
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

AssistantMessage AnthropicProvider::complete(const std::vector<Json>& messages, const Json& tools,
                                             const TextDelta& onText, CancelToken* cancel)
{
    Json payload = toAnthropicRequest(messages, tools, mSettings.vision);
    payload["model"] = mSettings.model;
    payload["max_tokens"] = mSettings.maxTokens;
    if (mSettings.temperature)
        payload["temperature"] = *mSettings.temperature;
    return exchange(payload, mSettings.stream, onText, cancel);
}

AssistantMessage AnthropicProvider::exchange(const Json& payload, bool stream, const TextDelta& onText,
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
                       {"anthropic-version", kAnthropicVersion},
                       {"User-Agent", "mcpchat"}};
    if (!mSettings.apiKey.empty())
        request.headers.push_back({"x-api-key", mSettings.apiKey});

    StreamBuilder builder;
    std::string failure;
    std::string errorBody;
    bool streaming = false;
    net::SseParser parser(
        [&](const std::string& data)
        {
            const Json event = Json::parse(data, nullptr, false);
            if (event.is_discarded())
            {
                failure = "Invalid event in the LLM stream: " + std::string(utf8Prefix(data, 200));
                return false;
            }
            // The API reports a failure mid-stream as an event of its own.
            if (event.value("type", "") == "error")
            {
                failure = errorMessage(0, data);
                return false;
            }
            const std::string text = builder.add(event);
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
    const AssistantMessage reply = fromWholeMessage(whole);
    if (!reply.content.empty() && onText)
        onText(reply.content);
    return reply;
}

} // namespace mcpchat
