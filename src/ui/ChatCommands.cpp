#include "ui/ChatCommands.h"

#include "util/Strings.h"

#include <algorithm>
#include <cctype>

namespace mcpchat
{

namespace
{
constexpr std::size_t kAttachLength = 7; // "/attach"

bool isSpace(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}
} // namespace

ChatCommand parseChatCommand(const std::string& text)
{
    ChatCommand out;
    const std::string line = trim(text);
    if (line == "/detach")
    {
        out.kind = ChatCommand::Kind::Detach;
        return out;
    }
    if (line.rfind("/attach", 0) != 0)
        return out;
    // "/attachment" is an ordinary message: the command has to be a word of its own.
    if (line.size() > kAttachLength && !isSpace(line[kAttachLength]))
        return out;
    std::string rest = trim(line.substr(std::min(kAttachLength, line.size())));
    if (rest.empty())
    {
        out.kind = ChatCommand::Kind::Usage;
        return out;
    }
    if (rest.front() == '"')
    {
        const std::size_t end = rest.find('"', 1);
        if (end == std::string::npos)
        {
            out.kind = ChatCommand::Kind::BadPath;
            out.error = "The quoted path is not closed.";
            return out;
        }
        out.kind = ChatCommand::Kind::Attach;
        out.path = rest.substr(1, end - 1);
        out.text = trim(rest.substr(end + 1));
        return out;
    }
    // An unquoted path ends at the first space, tab or line break, so a path pasted with the text after it still
    // reads as a path and not as one long name.
    out.kind = ChatCommand::Kind::Attach;
    const std::size_t end = rest.find_first_of(" \t\r\n");
    if (end == std::string::npos)
    {
        out.path = rest;
        return out;
    }
    out.path = rest.substr(0, end);
    out.text = trim(rest.substr(end));
    return out;
}

} // namespace mcpchat
