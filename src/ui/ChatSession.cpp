#include "ui/ChatSession.h"

#include "llm/AnthropicProvider.h"
#include "llm/OpenAiProvider.h"
#include "render/Image.h"
#include "util/Base64.h"
#include "util/Paths.h"
#include "util/Strings.h"

#include <fstream>
#include <iterator>

namespace mcpchat
{

namespace
{
// A whole conversation of screenshots is already a lot; one file should not dwarf it.
constexpr std::size_t kMaxAttachmentBytes = 16u * 1024u * 1024u;

std::string imageMimeType(const std::filesystem::path& path)
{
    const std::string extension = toLower(path.extension().u8string());
    if (extension == ".png")
        return "image/png";
    if (extension == ".jpg" || extension == ".jpeg")
        return "image/jpeg";
    if (extension == ".gif")
        return "image/gif";
    if (extension == ".bmp")
        return "image/bmp";
    return std::string();
}

bool decodeInto(std::vector<std::uint8_t>& bytes, const std::string& mimeType, const std::string& name,
                ChatAttachment& out, std::string& error)
{
    auto preview = std::make_shared<ChatImage>();
    if (!decodeImage(bytes, preview->rgba, preview->width, preview->height))
    {
        error = "not a PNG, JPEG, BMP or GIF image";
        return false;
    }
    out.name = name;
    out.mimeType = mimeType;
    out.bytes = std::move(bytes);
    out.preview = std::move(preview);
    return true;
}

// The provider the profile asks for; both carry the same settings apart from their shape.
std::unique_ptr<LlmProvider> makeProvider(const Profile& profile, const std::string& apiKey)
{
    if (profile.api == LlmApi::Anthropic)
    {
        AnthropicProvider::Settings settings;
        settings.baseUrl = profile.baseUrl;
        settings.model = profile.model;
        settings.apiKey = apiKey;
        settings.vision = profile.vision;
        settings.temperature = profile.temperature;
        settings.stream = profile.stream;
        settings.timeoutSeconds = profile.requestTimeout;
        return std::make_unique<AnthropicProvider>(settings);
    }
    OpenAiProvider::Settings settings;
    settings.baseUrl = profile.baseUrl;
    settings.model = profile.model;
    settings.apiKey = apiKey;
    settings.vision = profile.vision;
    settings.temperature = profile.temperature;
    settings.stream = profile.stream;
    settings.timeoutSeconds = profile.requestTimeout;
    return std::make_unique<OpenAiProvider>(settings);
}
} // namespace

bool loadAttachment(const std::filesystem::path& path, ChatAttachment& out, std::string& error)
{
    const std::string mimeType = imageMimeType(path);
    if (mimeType.empty())
    {
        error = "the name does not end in .png, .jpg, .jpeg, .gif or .bmp";
        return false;
    }
    std::error_code code;
    const std::uintmax_t size = std::filesystem::file_size(path, code);
    if (code)
    {
        error = code.message();
        return false;
    }
    if (size > kMaxAttachmentBytes)
    {
        error = "the file is larger than 16 MB";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "the file cannot be read";
        return false;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.empty())
    {
        error = "the file is empty";
        return false;
    }
    return decodeInto(bytes, mimeType, path.filename().u8string(), out, error);
}

bool loadAttachmentFromPng(std::vector<std::uint8_t> bytes, const std::string& name, ChatAttachment& out,
                           std::string& error)
{
    if (bytes.size() > kMaxAttachmentBytes)
    {
        error = "the image is larger than 16 MB";
        return false;
    }
    if (bytes.empty())
    {
        error = "the clipboard holds no image";
        return false;
    }
    return decodeInto(bytes, "image/png", name, out, error);
}

const std::map<std::string, ExposedTool>& ChatSession::HubProxy::tools() const
{
    return hub ? hub->tools() : mEmpty;
}

std::vector<std::pair<std::string, std::string>> ChatSession::HubProxy::instructions() const
{
    return hub ? hub->instructions() : std::vector<std::pair<std::string, std::string>>();
}

ToolResult ChatSession::HubProxy::call(const std::string& name, const Json& arguments, CancelToken* cancel)
{
    if (!hub)
        throw TransportError("no MCP server is connected");
    return hub->call(name, arguments, cancel);
}

bool ChatSession::ProviderSlot::supportsImages() const
{
    return current && current->supportsImages();
}

AssistantMessage ChatSession::ProviderSlot::complete(const std::vector<Json>& messages, const Json& tools,
                                                     const TextDelta& onText, CancelToken* cancel)
{
    if (!current)
        throw LlmError("no LLM profile is ready");
    return current->complete(messages, tools, onText, cancel);
}

ChatSession::ChatSession(std::filesystem::path configPath) : mConfigPath(std::move(configPath))
{
    mWorker = std::thread([this] { workerLoop(); });
}

ChatSession::~ChatSession()
{
    mCancel.cancel();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mQuit = true;
        mAnswered = true;
        mAllowed = false;
    }
    mWake.notify_all();
    if (mWorker.joinable())
        mWorker.join();
}

bool ChatSession::load(std::string& error)
{
    // The catalog is never fatal: a broken file leaves Settings with the built-in providers.
    std::string catalogProblem;
    loadCatalog(defaultCatalogPath(), mCatalog, catalogProblem);
    if (!catalogProblem.empty())
        notify(ChatEntry::Kind::Error, "Providers: " + catalogProblem + ". Using the built-in list.");

    std::error_code code;
    if (!std::filesystem::exists(mConfigPath, code))
    {
        mConfig = Config::example();
        if (!saveConfig(mConfigPath, mConfig, error))
            return false;
        notify(ChatEntry::Kind::Notice, "Wrote an example configuration to " + mConfigPath.u8string() +
                                                         ". Edit it in Settings.");
        return true;
    }
    if (!loadConfig(mConfigPath, mConfig, error))
        return false;
    const std::vector<std::string> problems = mConfig.problems();
    for (const std::string& problem : problems)
        notify(ChatEntry::Kind::Error, "Configuration: " + problem);
    return true;
}

bool ChatSession::apply(const Config& updated, std::string& error)
{
    if (!saveConfig(mConfigPath, updated, error))
        return false;
    const bool serversChanged = toJson(updated)["servers"] != toJson(mConfig)["servers"];
    mConfig = updated;
    if (serversChanged)
        connect();
    return true;
}

void ChatSession::post(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mJobs.push_back(std::move(job));
    }
    mWake.notify_all();
}

void ChatSession::deliver(std::function<void()> change)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mDelivered.push_back(std::move(change));
}

void ChatSession::workerLoop()
{
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mWake.wait(lock, [this] { return mQuit || !mJobs.empty(); });
            if (mQuit)
                break;
            job = std::move(mJobs.front());
            mJobs.pop_front();
            mBusy = true;
        }
        mCancel.reset();
        job();
        std::lock_guard<std::mutex> lock(mMutex);
        mBusy = !mJobs.empty();
    }
    mHub.hub.reset();
}

bool ChatSession::busy() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mBusy || !mJobs.empty();
}

bool ChatSession::pump()
{
    std::vector<std::function<void()>> changes;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        changes.swap(mDelivered);
    }
    for (const auto& change : changes)
        change();
    return !changes.empty();
}

void ChatSession::connect()
{
    const std::vector<ServerConfig> servers = mConfig.servers;
    mStatus = "Connecting to the MCP servers...";
    post([this, servers] { connectNow(servers); });
}

void ChatSession::notify(ChatEntry::Kind kind, std::string text)
{
    ChatEntry entry;
    entry.kind = kind;
    entry.text = std::move(text);
    mEntries.push_back(std::move(entry));
}

void ChatSession::connectNow(const std::vector<ServerConfig>& servers)
{
    auto hub = std::make_unique<ServerHub>(servers, &mSecrets);
    hub->connect(&mCancel);
    std::vector<ServerStatus> status = hub->status();
    mHub.hub = std::move(hub);
    std::size_t tools = mHub.tools().size();
    deliver([this, status, tools]
            {
                mServers = status;
                std::size_t live = 0;
                for (const ServerStatus& server : status)
                {
                    live += server.connected ? 1 : 0;
                    if (!server.connected)
                        notify(ChatEntry::Kind::Error, "Server '" + server.name + "': " + server.error);
                }
                mStatus = std::to_string(live) + " of " + std::to_string(status.size()) + " server(s) connected, " +
                          std::to_string(tools) + " tool(s).";
            });
}

AgentConfig ChatSession::agentConfig(const Config& config) const
{
    AgentConfig out;
    out.confirm = config.confirm;
    if (const Profile* profile = config.profile())
    {
        out.maxSteps = profile->maxSteps;
        out.simplifySchema = profile->simplifySchema;
        out.systemPrompt = profile->systemPrompt;
        out.prune.maxChars = profile->contextChars;
    }
    return out;
}

void ChatSession::notifyUser(const std::string& text, const std::vector<ChatAttachment>& attachments)
{
    ChatEntry entry;
    entry.kind = ChatEntry::Kind::User;
    entry.text = text;
    for (const ChatAttachment& attachment : attachments)
        entry.images.push_back(attachment.preview);
    mEntries.push_back(std::move(entry));
}

bool ChatSession::send(const std::string& text, const std::vector<ChatAttachment>& attachments)
{
    if (trim(text).empty() && attachments.empty())
        return false;
    const Config snapshot = mConfig;
    const Profile* profile = snapshot.profile();
    if (!profile)
    {
        notify(ChatEntry::Kind::Error, "No LLM profile is configured. Add one in Settings.");
        return false;
    }
    const std::vector<std::string> problems = profile->problems();
    if (!problems.empty())
    {
        notify(ChatEntry::Kind::Error, problems.front());
        return false;
    }
    if (!attachments.empty() && !profile->vision)
    {
        notify(ChatEntry::Kind::Error, "The profile '" + profile->name +
                                           "' is not set to see images, so the message was not sent. Turn on "
                                           "\"Images\" in Settings, or use a model that sees.");
        return false;
    }
    notifyUser(text, attachments);
    // A message with images is a list of parts; without them it stays plain text, as the API prefers.
    Json content = Json(text);
    if (!attachments.empty())
    {
        Json parts = Json::array();
        if (!trim(text).empty())
            parts.push_back({{"type", "text"}, {"text", text}});
        for (const ChatAttachment& attachment : attachments)
        {
            const std::string data = base64Encode(attachment.bytes.data(), attachment.bytes.size());
            parts.push_back({{"type", "image_url"},
                             {"image_url", {{"url", "data:" + attachment.mimeType + ";base64," + data}}}});
        }
        content = std::move(parts);
    }
    mStatus = "Waiting for " + profile->model + "...";
    const Profile chosen = *profile;
    const AgentConfig config = agentConfig(snapshot);
    post([this, content, chosen, config]
         {
             const std::string key = mSecrets.resolve(SecretStore::llmAccount(chosen.name), chosen.apiKeyEnv);
             try
             {
                 mProvider.current = makeProvider(chosen, key);
             }
             catch (const LlmError& error)
             {
                 const std::string message = error.what();
                 deliver([this, message] { notify(ChatEntry::Kind::Error, message); });
                 return;
             }
             if (!mAgent)
                 mAgent = std::make_unique<Agent>(mProvider, mHub, static_cast<AgentListener&>(*this), config,
                                                  [this](const std::string& name, const Json& arguments)
                                                  { return confirm(name, arguments); });
             mAgent->setConfig(config);
             deliver([this] { notify(ChatEntry::Kind::Assistant, std::string()); });
             const RunResult result = mAgent->run(content, &mCancel);
             const std::string ending = result.reason == RunReason::Cancelled ? "Stopped."
                                        : result.reason == RunReason::Done   ? "Done in " + std::to_string(result.steps) + " step(s)."
                                                                             : std::string("Stopped with an error.");
             deliver([this, ending, result]
                     {
                         if (!mEntries.empty() && mEntries.back().kind == ChatEntry::Kind::Assistant &&
                             trim(mEntries.back().text).empty())
                             mEntries.pop_back();
                         if (result.reason == RunReason::Cancelled)
                             notify(ChatEntry::Kind::Notice, "Stopped.");
                         mStatus = ending;
                     });
         });
    return true;
}

void ChatSession::stop()
{
    mCancel.cancel();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mAsking)
        {
            mAnswered = true;
            mAllowed = false;
        }
    }
    mWake.notify_all();
}

void ChatSession::releaseImages()
{
    for (const ChatEntry& entry : mEntries)
    {
        for (const auto& image : entry.images)
        {
            if (image->texture)
                mReleasedTextures.push_back(image->texture);
        }
    }
}

std::vector<std::uint64_t> ChatSession::takeReleasedTextures()
{
    std::vector<std::uint64_t> out;
    out.swap(mReleasedTextures);
    return out;
}

void ChatSession::newConversation()
{
    if (busy())
        return;
    releaseImages();
    mEntries.clear();
    mStatus.clear();
    post([this]
         {
             if (mAgent)
                 mAgent->reset();
         });
}

bool ChatSession::saveConversation(const std::filesystem::path& path, std::string& error)
{
    if (busy() || !mAgent)
    {
        error = busy() ? "wait for the current answer to finish" : "there is no conversation yet";
        return false;
    }
    return writeTextFileAtomic(path, mAgent->exportConversation().dump(2) + "\n", &error);
}

bool ChatSession::confirm(const std::string& name, const Json& arguments)
{
    std::unique_lock<std::mutex> lock(mMutex);
    mAsking = true;
    mAnswered = false;
    mAskName = name;
    mAskArguments = arguments.dump(2);
    mWake.wait(lock, [this] { return mAnswered || mQuit || mCancel.isSet(); });
    mAsking = false;
    return mAnswered && mAllowed && !mCancel.isSet();
}

bool ChatSession::pendingConfirmation(std::string& name, std::string& arguments) const
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mAsking || mAnswered)
        return false;
    name = mAskName;
    arguments = mAskArguments;
    return true;
}

void ChatSession::answerConfirmation(bool allow)
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mAnswered = true;
        mAllowed = allow;
    }
    mWake.notify_all();
}

void ChatSession::onStep(int step, int maxSteps)
{
    deliver([this, step, maxSteps]
            { mStatus = "Working... step " + std::to_string(step) + " of " + std::to_string(maxSteps); });
}

void ChatSession::onTextDelta(const std::string& text)
{
    deliver([this, text]
            {
                if (mEntries.empty() || mEntries.back().kind != ChatEntry::Kind::Assistant)
                    notify(ChatEntry::Kind::Assistant, std::string());
                mEntries.back().text += text;
            });
}

void ChatSession::onToolCall(const std::string& id, const std::string& name, const Json& arguments)
{
    const std::string shown = arguments.is_string() ? arguments.get<std::string>() : arguments.dump(2);
    deliver([this, id, name, shown]
            {
                if (!mEntries.empty() && mEntries.back().kind == ChatEntry::Kind::Assistant &&
                    trim(mEntries.back().text).empty())
                    mEntries.pop_back();
                ChatEntry entry;
                entry.kind = ChatEntry::Kind::Tool;
                entry.toolId = id;
                entry.toolName = name;
                entry.arguments = shown;
                mEntries.push_back(entry);
                mStatus = "Running " + name + "...";
            });
}

void ChatSession::onToolResult(const std::string& id, const std::string&, const std::string& text, bool isError,
                               const std::vector<ToolImage>& images)
{
    std::vector<std::shared_ptr<ChatImage>> decoded;
    for (const ToolImage& image : images)
    {
        auto view = std::make_shared<ChatImage>();
        if (decodeImage(image.bytes, view->rgba, view->width, view->height))
            decoded.push_back(view);
    }
    deliver([this, id, text, isError, decoded]
            {
                for (auto entry = mEntries.rbegin(); entry != mEntries.rend(); ++entry)
                {
                    if (entry->kind == ChatEntry::Kind::Tool && entry->toolId == id && !entry->finished)
                    {
                        entry->text = text;
                        entry->isError = isError;
                        entry->finished = true;
                        entry->images = decoded;
                        entry->expanded = isError || !decoded.empty();
                        break;
                    }
                }
                // The model's next words start a new bubble after the tool.
                notify(ChatEntry::Kind::Assistant, std::string());
            });
}

void ChatSession::onError(const std::string& message)
{
    deliver([this, message] { notify(ChatEntry::Kind::Error, message); });
}

} // namespace mcpchat
