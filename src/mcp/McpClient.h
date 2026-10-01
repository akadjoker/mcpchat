#ifndef MCPCHAT_MCP_CLIENT_H
#define MCPCHAT_MCP_CLIENT_H

#include "util/Json.h"

#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mcpchat
{

class CancelToken;

constexpr const char* kLatestProtocolVersion = "2025-11-25";

// A JSON-RPC error the server answered with.
class McpError : public std::runtime_error
{
public:
    McpError(int code, const std::string& message, Json data = Json())
        : std::runtime_error(message + " (code " + std::to_string(code) + ")"), code(code), message(message),
          data(std::move(data))
    {
    }
    int code;
    std::string message;
    Json data;
};

// The server could not be reached, or answered with something that is not JSON-RPC.
class TransportError : public std::runtime_error
{
public:
    explicit TransportError(const std::string& message, long httpStatus = 0)
        : std::runtime_error(message), httpStatus(httpStatus)
    {
    }
    long httpStatus;
};

class Transport
{
public:
    virtual ~Transport() = default;
    // The reply to a request, nothing for a notification. Throws TransportError.
    virtual std::optional<Json> send(const Json& message, CancelToken* cancel) = 0;
    virtual void setProtocolVersion(const std::string&)
    {
    }
    virtual void close()
    {
    }
};

struct ToolResult
{
    Json content = Json::array();
    bool isError = false;
    Json structured; // null when absent

    // The text blocks (and embedded text resources), one per line.
    std::string text() const;
    // The image blocks that carry data.
    std::vector<Json> images() const;
};

// The initialize handshake, tools/list and tools/call over any transport. Speaks protocol revisions 2024-11-05 to
// 2025-11-25.
class McpClient
{
public:
    McpClient(std::unique_ptr<Transport> transport, std::string clientName, std::string clientVersion);
    ~McpClient();

    Json initialize(CancelToken* cancel = nullptr);
    std::vector<Json> listTools(CancelToken* cancel = nullptr);
    ToolResult callTool(const std::string& name, const Json& arguments, CancelToken* cancel = nullptr);
    Json request(const std::string& method, const Json& params, CancelToken* cancel = nullptr);
    void notify(const std::string& method, const Json& params = Json());
    void close();

    const std::string& protocolVersion() const
    {
        return mProtocolVersion;
    }
    const Json& serverInfo() const
    {
        return mServerInfo;
    }
    const Json& capabilities() const
    {
        return mCapabilities;
    }
    const std::string& instructions() const
    {
        return mInstructions;
    }

    static bool supportsVersion(const std::string& version);

private:
    std::unique_ptr<Transport> mTransport;
    std::string mClientName;
    std::string mClientVersion;
    std::string mProtocolVersion;
    Json mServerInfo = Json::object();
    Json mCapabilities = Json::object();
    std::string mInstructions;
    std::atomic<long long> mNextId{1};
};

} // namespace mcpchat

#endif
