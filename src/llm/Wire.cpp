#include "llm/Wire.h"

#include "util/Strings.h"

#include <set>

namespace mcpchat
{

namespace
{
Json assistantToWire(const Json& message)
{
    const bool hasCalls = message.contains("tool_calls") && message["tool_calls"].is_array() &&
                          !message["tool_calls"].empty();
    const std::string content = message.contains("content") && message["content"].is_string()
                                    ? message["content"].get<std::string>()
                                    : std::string();
    Json wire = {{"role", "assistant"}, {"content", content.empty() && hasCalls ? Json() : Json(content)}};
    if (hasCalls)
        wire["tool_calls"] = message["tool_calls"];
    return wire;
}

// The words of a message built from parts, for a model that must not be sent the images.
std::string textOfParts(const Json& content)
{
    std::string text;
    for (const Json& part : content)
        if (part.value("type", "") == "text")
            text += part.value("text", std::string());
    return text;
}
} // namespace

Json toWireMessages(const std::vector<Json>& messages, bool vision)
{
    Json wire = Json::array();
    std::vector<Json> pendingImages;
    auto flushImages = [&]()
    {
        if (pendingImages.empty())
            return;
        const std::string text = pendingImages.size() > 1 ? "Images returned by the tool calls above."
                                                          : "Image returned by the tool call above.";
        Json parts = Json::array({{{"type", "text"}, {"text", text}}});
        for (const Json& image : pendingImages)
        {
            const std::string uri = "data:" + image.value("mimeType", std::string("image/png")) + ";base64," +
                                    image.value("data", std::string());
            parts.push_back({{"type", "image_url"}, {"image_url", {{"url", uri}}}});
        }
        wire.push_back({{"role", "user"}, {"content", parts}});
        pendingImages.clear();
    };

    for (const Json& message : messages)
    {
        const std::string role = message.value("role", "");
        if (role != "tool")
            flushImages();
        if (role == "assistant")
            wire.push_back(assistantToWire(message));
        else if (role == "tool")
        {
            wire.push_back({{"role", "tool"},
                            {"tool_call_id", message.value("tool_call_id", "")},
                            {"content", message.value("content", "")}});
            if (vision && message.contains("image") && message["image"].is_object())
                pendingImages.push_back(message["image"]);
        }
        else if (!vision && message.contains("content") && message["content"].is_array())
            wire.push_back({{"role", role}, {"content", textOfParts(message["content"])}});
        else
            wire.push_back({{"role", role}, {"content", message.contains("content") ? message["content"] : Json("")}});
    }
    flushImages();
    return wire;
}

Json assistantToHistory(const AssistantMessage& reply)
{
    Json message = {{"role", "assistant"}, {"content", reply.content}};
    if (!reply.toolCalls.empty())
    {
        Json calls = Json::array();
        for (const ToolCall& call : reply.toolCalls)
        {
            calls.push_back({{"id", call.id},
                             {"type", "function"},
                             {"function", {{"name", call.name},
                                           {"arguments", dump(call.arguments ? *call.arguments : Json::object())}}}});
        }
        message["tool_calls"] = calls;
    }
    return message;
}

void ReplyBuilder::setUsage(const Json& usage)
{
    Usage out;
    out.promptTokens = usage.value("prompt_tokens", 0LL);
    out.completionTokens = usage.value("completion_tokens", 0LL);
    mUsage = out;
}

void ReplyBuilder::addCallFragment(const Json& fragment, std::optional<int> index)
{
    if (!fragment.is_object())
        return;
    int at = 0;
    if (index)
        at = *index;
    else if (fragment.contains("index") && fragment["index"].is_number_integer())
        at = fragment["index"].get<int>();
    else
    {
        // No index: a fragment with an id starts a new call, one without continues the last.
        const bool hasId = fragment.contains("id") && fragment["id"].is_string() &&
                           !fragment["id"].get<std::string>().empty();
        at = hasId || mCalls.empty() ? static_cast<int>(mCalls.size()) : mCalls.rbegin()->first;
    }
    PartialCall& call = mCalls[at];
    if (fragment.contains("id") && fragment["id"].is_string() && !fragment["id"].get<std::string>().empty())
        call.id = fragment["id"].get<std::string>();
    if (!fragment.contains("function") || !fragment["function"].is_object())
        return;
    const Json& function = fragment["function"];
    if (function.contains("name") && function["name"].is_string())
        call.name += function["name"].get<std::string>();
    if (function.contains("arguments"))
    {
        const Json& arguments = function["arguments"];
        if (arguments.is_string())
            call.arguments += arguments.get<std::string>();
        else if (!arguments.is_null())
            call.arguments += dump(arguments);
    }
}

std::string ReplyBuilder::addChunk(const Json& chunk)
{
    if (!chunk.is_object())
        return std::string();
    if (chunk.contains("usage") && chunk["usage"].is_object())
        setUsage(chunk["usage"]);
    if (!chunk.contains("choices") || !chunk["choices"].is_array() || chunk["choices"].empty())
        return std::string();
    const Json& choice = chunk["choices"][0];
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string())
        mFinishReason = choice["finish_reason"].get<std::string>();
    if (!choice.contains("delta") || !choice["delta"].is_object())
        return std::string();
    const Json& delta = choice["delta"];
    if (delta.contains("tool_calls") && delta["tool_calls"].is_array())
    {
        for (const Json& fragment : delta["tool_calls"])
            addCallFragment(fragment, std::nullopt);
    }
    if (delta.contains("content") && delta["content"].is_string())
    {
        const std::string text = delta["content"].get<std::string>();
        mText += text;
        return text;
    }
    return std::string();
}

std::string ReplyBuilder::addMessage(const Json& response)
{
    if (!response.is_object())
        return std::string();
    if (response.contains("usage") && response["usage"].is_object())
        setUsage(response["usage"]);
    if (!response.contains("choices") || !response["choices"].is_array() || response["choices"].empty())
        return std::string();
    const Json& choice = response["choices"][0];
    mFinishReason = choice.contains("finish_reason") && choice["finish_reason"].is_string()
                        ? choice["finish_reason"].get<std::string>()
                        : std::string();
    if (!choice.contains("message") || !choice["message"].is_object())
        return std::string();
    const Json& message = choice["message"];
    if (message.contains("tool_calls") && message["tool_calls"].is_array())
    {
        int position = 0;
        for (const Json& call : message["tool_calls"])
            addCallFragment(call, position++);
    }
    if (message.contains("content") && message["content"].is_string())
    {
        const std::string text = message["content"].get<std::string>();
        mText += text;
        return text;
    }
    return std::string();
}

AssistantMessage ReplyBuilder::build() const
{
    AssistantMessage out;
    out.content = mText;
    out.finishReason = mFinishReason;
    out.usage = mUsage;
    std::set<std::string> seen;
    for (const auto& entry : mCalls)
    {
        std::string id = entry.second.id.empty() ? "call_" + std::to_string(entry.first) : entry.second.id;
        // Some servers reuse ids; tool results are matched by id.
        while (seen.count(id))
            id += "_";
        seen.insert(id);
        out.toolCalls.push_back(parseToolCall(id, entry.second.name, entry.second.arguments));
    }
    return out;
}

ToolCall parseToolCall(const std::string& id, const std::string& name, const std::string& raw)
{
    ToolCall call;
    call.id = id;
    call.name = name;
    call.rawArguments = raw;
    if (name.empty())
    {
        call.error = "the tool call has no function name";
        return call;
    }
    if (trim(raw).empty())
    {
        // Models send "" for commands without arguments.
        call.arguments = Json::object();
        return call;
    }
    const Json arguments = Json::parse(raw, nullptr, false);
    if (arguments.is_discarded())
    {
        call.error = "the arguments are not valid JSON";
        return call;
    }
    if (!arguments.is_object())
    {
        call.error = "the arguments must be a JSON object";
        return call;
    }
    call.arguments = arguments;
    return call;
}

} // namespace mcpchat
