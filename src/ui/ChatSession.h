#ifndef MCPCHAT_CHAT_SESSION_H
#define MCPCHAT_CHAT_SESSION_H

#include "agent/Agent.h"
#include "config/Config.h"
#include "mcp/ServerHub.h"
#include "util/Cancel.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mcpchat
{

class LlmProvider;

struct ChatImage
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
    std::uint64_t texture = 0; // made by the window on first draw
};

// An image the user brings into the conversation: the file's own bytes go to the model, the decoded pixels to the
// window. `preview` is shared with the entry the message makes. `path` is the file, absolute, for the tools that
// read one; it is empty when the image has no file.
struct ChatAttachment
{
    std::string name;
    std::string path;
    std::string mimeType;
    std::vector<std::uint8_t> bytes;
    std::shared_ptr<ChatImage> preview;
};

// Reads an image file. False with `error` set when it is missing, unreadable, too big or not an image we decode.
bool loadAttachment(const std::filesystem::path& path, ChatAttachment& out, std::string& error);
// Same, from PNG bytes already in memory (the clipboard). They are also written to a file in the temporary
// directory, so this image has a path too; it goes without one when the file cannot be written.
bool loadAttachmentFromPng(std::vector<std::uint8_t> bytes, const std::string& name, ChatAttachment& out,
                           std::string& error);

struct ChatEntry
{
    enum class Kind
    {
        User,
        Assistant,
        Tool,
        Error,
        Notice
    };

    Kind kind = Kind::Notice;
    std::string text;
    std::string toolId;
    std::string toolName;
    std::string arguments;
    bool isError = false;
    bool finished = false;
    bool expanded = false;
    std::vector<std::shared_ptr<ChatImage>> images;
};

// The conversation and everything behind it. Network, tools and the model run on one worker thread; what they
// produce reaches the window through pump(), on the window's thread.
class ChatSession : private AgentListener
{
public:
    explicit ChatSession(std::filesystem::path configPath);
    ~ChatSession();

    // Reads the configuration, writing the example one on first run.
    bool load(std::string& error);
    const Config& config() const
    {
        return mConfig;
    }
    // The providers Settings offers, from `providers.json`; never empty.
    const ProviderCatalog& catalog() const
    {
        return mCatalog;
    }
    const std::filesystem::path& configPath() const
    {
        return mConfigPath;
    }
    // Saves and starts using `updated`; reconnects when the servers changed.
    bool apply(const Config& updated, std::string& error);

    void connect();
    // A line from the window itself (a failed save, a bad start), shown in the conversation.
    void notify(ChatEntry::Kind kind, std::string text);
    // `attachments` ride with the message; they need a profile that sees images, and `text` may be empty when they
    // are given. False when nothing was sent, so the caller can keep what it typed.
    bool send(const std::string& text, const std::vector<ChatAttachment>& attachments = {});
    void stop();
    void newConversation();
    bool busy() const;
    bool saveConversation(const std::filesystem::path& path, std::string& error);

    // Applies what the worker produced since the last call; true when anything changed.
    bool pump();
    std::vector<ChatEntry>& entries()
    {
        return mEntries;
    }
    const std::vector<ServerStatus>& servers() const
    {
        return mServers;
    }
    const std::string& status() const
    {
        return mStatus;
    }

    // A tool waiting for the user's yes or no.
    bool pendingConfirmation(std::string& name, std::string& arguments) const;
    void answerConfirmation(bool allow);

    SecretStore& secrets()
    {
        return mSecrets;
    }
    // Images that left the conversation; the window frees their textures.
    std::vector<std::uint64_t> takeReleasedTextures();

private:
    // Forwards to whichever hub is current, so the agent keeps one ToolHost across reconnects.
    class HubProxy : public ToolHost
    {
    public:
        const std::map<std::string, ExposedTool>& tools() const override;
        std::vector<std::pair<std::string, std::string>> instructions() const override;
        ToolResult call(const std::string& name, const Json& arguments, CancelToken* cancel) override;
        std::unique_ptr<ServerHub> hub;

    private:
        std::map<std::string, ExposedTool> mEmpty;
    };

    // The agent keeps one provider; this forwards to the one built for the current profile.
    class ProviderSlot : public LlmProvider
    {
    public:
        bool supportsImages() const override;
        AssistantMessage complete(const std::vector<Json>& messages, const Json& tools, const TextDelta& onText,
                                  CancelToken* cancel) override;
        std::unique_ptr<LlmProvider> current;
    };

    void post(std::function<void()> job);
    void deliver(std::function<void()> change);
    void workerLoop();
    void connectNow(const std::vector<ServerConfig>& servers);
    AgentConfig agentConfig(const Config& config) const;
    bool confirm(const std::string& name, const Json& arguments);
    void releaseImages();
    // The user's own line in the conversation, with whatever images came with it.
    void notifyUser(const std::string& text, const std::vector<ChatAttachment>& attachments);

    void onStep(int step, int maxSteps) override;
    void onTextDelta(const std::string& text) override;
    void onToolCall(const std::string& id, const std::string& name, const Json& arguments) override;
    void onToolResult(const std::string& id, const std::string& name, const std::string& text, bool isError,
                      const std::vector<ToolImage>& images) override;
    void onError(const std::string& message) override;

    std::filesystem::path mConfigPath;
    Config mConfig;
    ProviderCatalog mCatalog;
    SecretStore mSecrets;

    // Window thread.
    std::vector<ChatEntry> mEntries;
    std::vector<ServerStatus> mServers;
    std::string mStatus;
    std::vector<std::uint64_t> mReleasedTextures;

    // Worker thread.
    HubProxy mHub;
    ProviderSlot mProvider;
    std::unique_ptr<Agent> mAgent;
    CancelToken mCancel;

    std::thread mWorker;
    mutable std::mutex mMutex;
    std::condition_variable mWake;
    std::deque<std::function<void()>> mJobs;
    std::vector<std::function<void()>> mDelivered;
    bool mQuit = false;
    bool mBusy = false;

    bool mAsking = false;
    bool mAnswered = false;
    bool mAllowed = false;
    std::string mAskName;
    std::string mAskArguments;
};

} // namespace mcpchat

#endif
