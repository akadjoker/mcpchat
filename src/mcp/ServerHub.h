#ifndef MCPCHAT_SERVER_HUB_H
#define MCPCHAT_SERVER_HUB_H

#include "config/Config.h"
#include "mcp/McpClient.h"
#include "mcp/ToolHost.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mcpchat
{

// Where keys and tokens come from: the configured environment variable, else a value typed in this session (kept in
// memory only, never written).
class SecretStore
{
public:
    static std::string llmAccount(const std::string& profile)
    {
        return "llm:" + profile;
    }
    static std::string serverAccount(const std::string& server)
    {
        return "mcp:" + server;
    }

    std::string resolve(const std::string& account, const std::string& environmentVariable) const;
    void setMemory(const std::string& account, const std::string& value);
    bool inMemory(const std::string& account) const;

private:
    mutable std::mutex mMutex;
    std::map<std::string, std::string> mMemory;
};

struct ServerStatus
{
    std::string name;
    bool connected = false;
    std::string error;
    std::string title;
    std::vector<std::string> tools;
};

// Every enabled MCP server behind one tool list: the model sees `tool` with one server and `server__tool` with
// several.
class ServerHub : public ToolHost
{
public:
    // Builds the transport for a server; tests swap in their own.
    using TransportFactory = std::function<std::unique_ptr<Transport>(const ServerConfig&)>;

    ServerHub(std::vector<ServerConfig> servers, const SecretStore* secrets, TransportFactory factory = TransportFactory());
    ~ServerHub();

    // A server that fails is reported, the others still work.
    void connect(CancelToken* cancel = nullptr);
    void close();

    const std::map<std::string, ExposedTool>& tools() const override
    {
        return mTools;
    }
    std::vector<ServerStatus> status() const;
    // (title, text) of every connected server that gave instructions.
    std::vector<std::pair<std::string, std::string>> instructions() const override;
    ToolResult call(const std::string& exposedName, const Json& arguments, CancelToken* cancel) override;

    static std::string exposedName(const std::string& server, const std::string& tool, bool prefixed);
    static std::unique_ptr<Transport> openTransport(const ServerConfig& server, const SecretStore* secrets);

private:
    struct Connected
    {
        std::string name;
        std::unique_ptr<McpClient> client;
        std::string error;
        std::vector<Json> tools;
    };

    void index();

    std::vector<ServerConfig> mConfigs;
    const SecretStore* mSecrets;
    TransportFactory mFactory;
    std::vector<Connected> mServers;
    std::map<std::string, ExposedTool> mTools;
};

} // namespace mcpchat

#endif
