#ifndef MCPCHAT_AGENT_H
#define MCPCHAT_AGENT_H

#include "agent/History.h"
#include "config/Config.h"
#include "llm/Provider.h"
#include "mcp/ToolHost.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mcpchat
{

class CancelToken;

struct AgentConfig
{
    int maxSteps = 40;
    PruneLimits prune;
    std::size_t maxResultChars = 8000;
    bool simplifySchema = false;
    std::string systemPrompt;
    ConfirmPolicy confirm = ConfirmPolicy::Destructive;
};

struct ToolImage
{
    std::string mimeType;
    std::vector<std::uint8_t> bytes;
};

class AgentListener
{
public:
    virtual ~AgentListener() = default;
    virtual void onStep(int step, int maxSteps)
    {
        (void)step;
        (void)maxSteps;
    }
    virtual void onTextDelta(const std::string& text)
    {
        (void)text;
    }
    virtual void onToolCall(const std::string& id, const std::string& name, const Json& arguments)
    {
        (void)id;
        (void)name;
        (void)arguments;
    }
    virtual void onToolResult(const std::string& id, const std::string& name, const std::string& text, bool isError,
                              const std::vector<ToolImage>& images)
    {
        (void)id;
        (void)name;
        (void)text;
        (void)isError;
        (void)images;
    }
    virtual void onError(const std::string& message)
    {
        (void)message;
    }
};

enum class RunReason
{
    Done,
    Cancelled,
    MaxSteps,
    Error
};

struct RunResult
{
    RunReason reason = RunReason::Done;
    int steps = 0;
};

extern const char* const kDefaultSystemPrompt;

// The conversation loop: the model answers or asks for tools, the tools run on their MCP servers, the results go back
// to the model, until it answers without asking for more.
class Agent
{
public:
    // Asked before a tool the confirm policy covers runs; false declines it.
    using Confirm = std::function<bool(const std::string& name, const Json& arguments)>;

    Agent(LlmProvider& provider, ToolHost& host, AgentListener& listener, AgentConfig config, Confirm confirm);

    // `userContent` is the message text, or a list of parts (text and image_url) when images ride along.
    RunResult run(const Json& userContent, CancelToken* cancel);
    void reset();
    // Takes effect from the next run; the conversation is kept.
    void setConfig(AgentConfig config)
    {
        mConfig = std::move(config);
    }

    std::string systemPrompt() const;
    Json toolDefinitions() const;
    const std::vector<Json>& messages() const
    {
        return mMessages;
    }
    // The conversation with image data left out, for saving.
    Json exportConversation() const;

private:
    struct Outcome
    {
        Outcome() = default;
        Outcome(std::string message, bool error = false, bool unreachable = false)
            : text(std::move(message)), isError(error), fatal(unreachable)
        {
        }

        std::string text;
        bool isError = false;
        std::vector<ToolImage> images;
        Json image; // the block sent to a model that can see, null otherwise
        bool fatal = false; // the server is unreachable: no point in carrying on
    };

    // False with `stop` set when the turn must end.
    bool runToolCalls(const std::vector<ToolCall>& calls, CancelToken* cancel, RunReason& stop);
    Outcome execute(const ToolCall& call, CancelToken* cancel);
    Outcome attempt(const ToolCall& call, CancelToken* cancel);
    bool needsConfirmation(const ExposedTool& tool) const;
    void addToolMessage(const std::string& id, const Outcome& outcome);

    LlmProvider& mProvider;
    ToolHost& mHost;
    AgentListener& mListener;
    AgentConfig mConfig;
    Confirm mConfirm;
    std::vector<Json> mMessages;
};

} // namespace mcpchat

#endif
