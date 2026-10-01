#ifndef MCPCHAT_TOOL_HOST_H
#define MCPCHAT_TOOL_HOST_H

#include "mcp/McpClient.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mcpchat
{

struct ExposedTool
{
    std::string name;   // what the model calls
    std::string server;
    Json tool;          // the MCP Tool

    bool readOnly() const;
    // MCP's default for a tool that is not read-only is destructive; the confirm policy decides whether that counts.
    bool destructive() const;
};

// What the agent needs from the MCP side.
class ToolHost
{
public:
    virtual ~ToolHost() = default;
    virtual const std::map<std::string, ExposedTool>& tools() const = 0;
    // (title, text) of every server that gave instructions.
    virtual std::vector<std::pair<std::string, std::string>> instructions() const = 0;
    // Throws McpError, TransportError or Cancelled.
    virtual ToolResult call(const std::string& exposedName, const Json& arguments, CancelToken* cancel) = 0;
};

} // namespace mcpchat

#endif
