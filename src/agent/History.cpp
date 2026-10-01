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

std::size_t sizeOf(const std::vector<Json>& messages)
{
    std::size_t total = 0;
    for (const Json& message : messages)
    {
        total += contentOf(message).size();
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
    std::vector<Json*> withImage;
    for (Json& message : messages)
    {
        if (message.value("role", "") == "tool" && message.contains("image"))
            withImage.push_back(&message);
    }
    for (std::size_t i = 0; i + limits.keepImages < withImage.size(); ++i)
    {
        withImage[i]->erase("image");
        (*withImage[i])["content"] = contentOf(*withImage[i]) + "\n[screenshot no longer attached]";
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
