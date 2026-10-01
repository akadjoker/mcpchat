#include "mcp/McpClient.h"

namespace mcpchat
{

std::string ToolResult::text() const
{
    std::string out;
    auto add = [&out](const std::string& part)
    {
        if (part.empty())
            return;
        if (!out.empty())
            out += '\n';
        out += part;
    };
    for (const Json& block : content)
    {
        if (block.is_object() && block.value("type", "") == "text")
            add(block.value("text", ""));
    }
    for (const Json& block : content)
    {
        if (block.is_object() && block.value("type", "") == "resource" && block.contains("resource") &&
            block["resource"].is_object())
            add(block["resource"].value("text", ""));
    }
    return out;
}

std::vector<Json> ToolResult::images() const
{
    std::vector<Json> out;
    for (const Json& block : content)
    {
        if (block.is_object() && block.value("type", "") == "image" && block.contains("data") &&
            block["data"].is_string() && !block["data"].get<std::string>().empty())
            out.push_back(block);
    }
    return out;
}

McpClient::McpClient(std::unique_ptr<Transport> transport, std::string clientName, std::string clientVersion)
    : mTransport(std::move(transport)), mClientName(std::move(clientName)), mClientVersion(std::move(clientVersion))
{
}

McpClient::~McpClient()
{
    close();
}

bool McpClient::supportsVersion(const std::string& version)
{
    return version == "2024-11-05" || version == "2025-03-26" || version == "2025-06-18" || version == "2025-11-25";
}

Json McpClient::initialize(CancelToken* cancel)
{
    const Json result = request("initialize",
                                {{"protocolVersion", kLatestProtocolVersion},
                                 {"capabilities", Json::object()},
                                 {"clientInfo", {{"name", mClientName}, {"version", mClientVersion}}}},
                                cancel);
    const std::string version = result.value("protocolVersion", "");
    if (!supportsVersion(version))
        throw TransportError("the server wants MCP protocol version '" + version +
                             "', which this client does not speak");
    mProtocolVersion = version;
    mServerInfo = result.contains("serverInfo") && result["serverInfo"].is_object() ? result["serverInfo"]
                                                                                    : Json::object();
    mCapabilities = result.contains("capabilities") && result["capabilities"].is_object() ? result["capabilities"]
                                                                                          : Json::object();
    mInstructions = result.contains("instructions") && result["instructions"].is_string()
                        ? result["instructions"].get<std::string>()
                        : std::string();
    mTransport->setProtocolVersion(version);
    notify("notifications/initialized");
    return result;
}

std::vector<Json> McpClient::listTools(CancelToken* cancel)
{
    std::vector<Json> tools;
    if (!mCapabilities.contains("tools"))
        return tools;
    std::string cursor;
    // A server that never stops paging is a bug, not a reason to hang.
    for (int page = 0; page < 1000; ++page)
    {
        Json params = Json::object();
        if (!cursor.empty())
            params["cursor"] = cursor;
        const Json result = request("tools/list", params, cancel);
        if (result.contains("tools") && result["tools"].is_array())
        {
            for (const Json& tool : result["tools"])
            {
                if (tool.is_object() && tool.contains("name") && tool["name"].is_string())
                    tools.push_back(tool);
            }
        }
        cursor = result.contains("nextCursor") && result["nextCursor"].is_string()
                     ? result["nextCursor"].get<std::string>()
                     : std::string();
        if (cursor.empty())
            break;
    }
    return tools;
}

ToolResult McpClient::callTool(const std::string& name, const Json& arguments, CancelToken* cancel)
{
    const Json result = request("tools/call",
                                {{"name", name}, {"arguments", arguments.is_object() ? arguments : Json::object()}},
                                cancel);
    ToolResult out;
    if (result.contains("content") && result["content"].is_array())
        out.content = result["content"];
    out.isError = result.value("isError", false);
    if (result.contains("structuredContent") && result["structuredContent"].is_object())
        out.structured = result["structuredContent"];
    return out;
}

Json McpClient::request(const std::string& method, const Json& params, CancelToken* cancel)
{
    const long long id = mNextId++;
    Json message = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.is_null())
        message["params"] = params;
    const std::optional<Json> reply = mTransport->send(message, cancel);
    if (!reply || !reply->is_object() || !reply->contains("id") || (*reply)["id"] != Json(id))
        throw TransportError("no answer to " + method);
    if (reply->contains("error"))
    {
        const Json& error = (*reply)["error"];
        const int code = error.is_object() && error.contains("code") && error["code"].is_number_integer()
                             ? error["code"].get<int>()
                             : 0;
        const std::string text = error.is_object() ? error.value("message", "error") : std::string("error");
        throw McpError(code, text, error.is_object() && error.contains("data") ? error["data"] : Json());
    }
    return reply->contains("result") && (*reply)["result"].is_object() ? (*reply)["result"] : Json::object();
}

void McpClient::notify(const std::string& method, const Json& params)
{
    Json message = {{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.is_null())
        message["params"] = params;
    mTransport->send(message, nullptr);
}

void McpClient::close()
{
    if (mTransport)
        mTransport->close();
}

} // namespace mcpchat
