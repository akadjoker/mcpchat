#ifndef MCPCHAT_WIRE_H
#define MCPCHAT_WIRE_H

#include "llm/Provider.h"

#include <map>

namespace mcpchat
{

// The conversation is kept as chat-completions messages, except that a tool message may carry
// "image": {"mimeType", "data"}.

// Conversation -> request `messages`. An image cannot ride in a `tool` message, so it follows the run of tool
// messages as a `user` data-URI message (a user message in between would break the tool_call/tool pairing).
Json toWireMessages(const std::vector<Json>& messages, bool vision);

// The assistant reply as stored in the conversation; arguments are normalised JSON ("{}" when unusable), since
// servers would reject the original text.
Json assistantToHistory(const AssistantMessage& reply);

// Accumulates a reply from stream deltas or from one whole response; tool-call arguments arrive in fragments per
// `index`.
class ReplyBuilder
{
public:
    // Returns the text the chunk added.
    std::string addChunk(const Json& chunk);
    std::string addMessage(const Json& response);
    AssistantMessage build() const;

private:
    struct PartialCall
    {
        std::string id;
        std::string name;
        std::string arguments;
    };
    void addCallFragment(const Json& fragment, std::optional<int> index);
    void setUsage(const Json& usage);

    std::string mText;
    std::map<int, PartialCall> mCalls;
    std::string mFinishReason;
    std::optional<Usage> mUsage;
};

ToolCall parseToolCall(const std::string& id, const std::string& name, const std::string& raw);

} // namespace mcpchat

#endif
