#include "mcp/ServerHub.h"

#include "mcp/HttpTransport.h"
#include "mcp/StdioTransport.h"
#include "util/Cancel.h"
#include "util/Paths.h"
#include "util/Strings.h"

#include <cstdio>

namespace mcpchat
{

namespace
{
// What OpenAI-compatible servers accept for a function name.
constexpr std::size_t kNameLimit = 64;
} // namespace

std::string SecretStore::resolve(const std::string& account, const std::string& environmentVariable) const
{
    if (!environmentVariable.empty())
    {
        const std::string value = environment(environmentVariable);
        if (!value.empty())
            return value;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    const auto found = mMemory.find(account);
    return found == mMemory.end() ? std::string() : found->second;
}

void SecretStore::setMemory(const std::string& account, const std::string& value)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (value.empty())
        mMemory.erase(account);
    else
        mMemory[account] = value;
}

bool SecretStore::inMemory(const std::string& account) const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mMemory.count(account) != 0;
}

bool ExposedTool::readOnly() const
{
    return tool.contains("annotations") && tool["annotations"].is_object() &&
           tool["annotations"].value("readOnlyHint", Json(false)) == Json(true);
}

bool ExposedTool::destructive() const
{
    return tool.contains("annotations") && tool["annotations"].is_object() &&
           tool["annotations"].value("destructiveHint", Json(false)) == Json(true);
}

ServerHub::ServerHub(std::vector<ServerConfig> servers, const SecretStore* secrets, TransportFactory factory)
    : mSecrets(secrets), mFactory(std::move(factory))
{
    for (ServerConfig& server : servers)
    {
        if (server.enabled)
            mConfigs.push_back(std::move(server));
    }
    if (!mFactory)
        mFactory = [this](const ServerConfig& server) { return openTransport(server, mSecrets); };
}

ServerHub::~ServerHub()
{
    close();
}

std::unique_ptr<Transport> ServerHub::openTransport(const ServerConfig& server, const SecretStore* secrets)
{
    if (!server.url.empty())
    {
        std::vector<net::Header> headers;
        bool hasAuthorization = false;
        for (const auto& header : server.headers)
        {
            headers.push_back({header.first, header.second});
            hasAuthorization = hasAuthorization || toLower(header.first) == "authorization";
        }
        std::string token;
        if (secrets)
            token = secrets->resolve(SecretStore::serverAccount(server.name), server.tokenEnv);
        else if (!server.tokenEnv.empty())
            token = environment(server.tokenEnv);
        if (!token.empty() && !hasAuthorization)
            headers.push_back({"Authorization", "Bearer " + token});
        return std::make_unique<HttpTransport>(server.url, headers, server.timeout);
    }
    return std::make_unique<StdioTransport>(server.command, server.args, server.env, server.timeout);
}

std::string ServerHub::exposedName(const std::string& server, const std::string& tool, bool prefixed)
{
    const std::string raw = prefixed ? server + "__" + tool : tool;
    std::string name = raw;
    for (char& c : name)
    {
        const bool good = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                          c == '-';
        if (!good)
            c = '_';
    }
    if (name.size() > kNameLimit)
    {
        // FNV-1a of the full name keeps two long names apart.
        unsigned long long hash = 1469598103934665603ull;
        for (const char c : raw)
            hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        char digest[17];
        std::snprintf(digest, sizeof(digest), "%016llx", hash);
        name = name.substr(0, kNameLimit - 9) + "_" + std::string(digest, 8);
    }
    return name;
}

void ServerHub::connect(CancelToken* cancel)
{
    close();
    for (const ServerConfig& config : mConfigs)
    {
        Connected entry;
        entry.name = config.name;
        try
        {
            auto client = std::make_unique<McpClient>(mFactory(config), "mcpchat", MCPCHAT_VERSION);
            client->initialize(cancel);
            entry.tools = client->listTools(cancel);
            entry.client = std::move(client);
        }
        catch (const McpError& error)
        {
            entry.error = error.what();
        }
        catch (const TransportError& error)
        {
            entry.error = error.what();
        }
        catch (const Cancelled&)
        {
            entry.error = "cancelled";
        }
        mServers.push_back(std::move(entry));
    }
    index();
}

void ServerHub::index()
{
    mTools.clear();
    std::size_t live = 0;
    for (const Connected& server : mServers)
        live += server.client ? 1 : 0;
    for (const Connected& server : mServers)
    {
        if (!server.client)
            continue;
        for (const Json& tool : server.tools)
        {
            const std::string name = exposedName(server.name, tool["name"].get<std::string>(), live > 1);
            mTools[name] = ExposedTool{name, server.name, tool};
        }
    }
}

std::vector<ServerStatus> ServerHub::status() const
{
    std::vector<ServerStatus> out;
    for (const Connected& server : mServers)
    {
        ServerStatus item;
        item.name = server.name;
        item.connected = server.client != nullptr;
        item.error = server.error;
        if (server.client)
        {
            const Json& info = server.client->serverInfo();
            item.title = info.value("title", info.value("name", server.name));
        }
        for (const auto& tool : mTools)
        {
            if (tool.second.server == server.name)
                item.tools.push_back(tool.first);
        }
        out.push_back(item);
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> ServerHub::instructions() const
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const Connected& server : mServers)
    {
        if (!server.client || trim(server.client->instructions()).empty())
            continue;
        const Json& info = server.client->serverInfo();
        out.push_back({info.value("title", info.value("name", server.name)), trim(server.client->instructions())});
    }
    return out;
}

ToolResult ServerHub::call(const std::string& exposedName, const Json& arguments, CancelToken* cancel)
{
    const auto tool = mTools.find(exposedName);
    if (tool == mTools.end())
        throw McpError(-32602, "unknown tool " + exposedName);
    for (Connected& server : mServers)
    {
        if (server.name == tool->second.server && server.client)
            return server.client->callTool(tool->second.tool["name"].get<std::string>(), arguments, cancel);
    }
    throw TransportError("server " + tool->second.server + " is not connected");
}

void ServerHub::close()
{
    for (Connected& server : mServers)
    {
        if (server.client)
            server.client->close();
    }
    mServers.clear();
    mTools.clear();
}

} // namespace mcpchat
