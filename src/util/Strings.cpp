#include "util/Strings.h"

#include <cctype>

namespace mcpchat
{

std::string trim(std::string_view text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return std::string(text.substr(begin, end - begin));
}

bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string toLower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view utf8Prefix(std::string_view text, std::size_t bytes)
{
    if (text.size() <= bytes)
        return text;
    std::size_t cut = bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;
    return text.substr(0, cut);
}

std::string truncate(const std::string& text, std::size_t limit)
{
    if (text.size() <= limit)
        return text;
    const std::string_view kept = utf8Prefix(text, limit);
    return std::string(kept) + "\n[truncated: " + std::to_string(text.size() - kept.size()) + " more bytes]";
}

} // namespace mcpchat
