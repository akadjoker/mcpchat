#ifndef MCPCHAT_CHAT_COMMANDS_H
#define MCPCHAT_CHAT_COMMANDS_H

#include <string>

namespace mcpchat
{

// What one thing typed in the input box means. The text may hold more than the command: whatever follows it becomes
// `text`, so a command and a question pasted together still work.
struct ChatCommand
{
    enum class Kind
    {
        None,    // an ordinary message, to send as typed
        Attach,  // /attach <path>, with the `text` to send along with the image
        Detach,  // /detach
        Usage,   // /attach with nothing after it
        BadPath  // /attach with a path that cannot be read as one; `error` says why
    };

    Kind kind = Kind::None;
    std::string path;
    std::string text;
    std::string error;
};

// Reads the command at the start of the text. A path holds no whitespace unless it is quoted with ".
ChatCommand parseChatCommand(const std::string& text);

} // namespace mcpchat

#endif
