#include "agent/History.h"

#include "util/Strings.h"

namespace mcpchat
{

namespace
{
const char* const kOmitted = "[...omitted from history]";

std::string contentOf(const Json& message)
{
    return message.contains("content") && message["content"].is_string() ? message["content"].get<std::string>()
                                                                         : std::string();
}

// An image the user attached rides as an image_url part of the message.
bool isImagePart(const Json& part)
{
    return part.is_object() && part.value("type", "") == "image_url";
}

bool hasImageParts(const Json& message)
{
    if (!message.contains("content") || !message["content"].is_array())
        return false;
    for (const Json& part : message["content"])
        if (isImagePart(part))
            return true;
    return false;
}

// Leaves the words of a user message behind, the way the tool messages get a note.
void dropImageParts(Json& message)
{
    std::string text;
    Json others = Json::array();
    for (const Json& part : message["content"])
    {
        if (isImagePart(part))
            continue;
        if (part.value("type", "") == "text" && text.empty())
            text = part.value("text", std::string());
        else
            others.push_back(part);
    }
    if (!text.empty())
        text += "\n";
    text += "[attached image no longer in history]";
    if (others.empty())
    {
        message["content"] = text;
        return;
    }
    others.insert(others.begin(), {{"type", "text"}, {"text", text}});
    message["content"] = std::move(others);
}

std::size_t sizeOf(const std::vector<Json>& messages)
{
    std::size_t total = 0;
    for (const Json& message : messages)
    {
        total += contentOf(message).size();
        if (message.contains("content") && message["content"].is_array())
        {
            for (const Json& part : message["content"])
                if (part.value("type", "") == "text")
                    total += part.value("text", std::string()).size();
        }
        if (message.contains("tool_calls") && message["tool_calls"].is_array())
        {
            for (const Json& call : message["tool_calls"])
                total += call["function"].value("arguments", std::string()).size();
        }
    }
    return total;
}
} // namespace

void pruneHistory(std::vector<Json>& messages, const PruneLimits& limits)
{
    // Screenshots and attached images count against the same budget, oldest first.
    std::vector<Json*> withImage;
    for (Json& message : messages)
    {
        if (message.value("role", "") == "tool" && message.contains("image"))
            withImage.push_back(&message);
        else if (hasImageParts(message))
            withImage.push_back(&message);
    }
    for (std::size_t i = 0; i + limits.keepImages < withImage.size(); ++i)
    {
        Json& message = *withImage[i];
        if (message.contains("image"))
        {
            message.erase("image");
            message["content"] = contentOf(message) + "\n[screenshot no longer attached]";
        }
        else
            dropImageParts(message);
    }

    std::vector<Json*> results;
    for (Json& message : messages)
    {
        if (message.value("role", "") == "tool")
            results.push_back(&message);
    }
    for (std::size_t i = 0; i + limits.recentResults < results.size(); ++i)
    {
        const std::string content = contentOf(*results[i]);
        if (content.size() > limits.oldResultChars && content.find(kOmitted) == std::string::npos)
            (*results[i])["content"] = std::string(utf8Prefix(content, limits.oldResultChars)) + "\n" + kOmitted;
    }

    while (sizeOf(messages) > limits.maxChars)
    {
        std::size_t second = 0;
        std::size_t users = 0;
        for (std::size_t i = 0; i < messages.size(); ++i)
        {
            if (messages[i].value("role", "") == "user" && ++users == 2)
            {
                second = i;
                break;
            }
        }
        if (users < 2)
            break;
        messages.erase(messages.begin(), messages.begin() + static_cast<std::ptrdiff_t>(second));
    }
}

} // namespace mcpchat
