#ifndef MCPCHAT_FAKE_LOGIC_H
#define MCPCHAT_FAKE_LOGIC_H

#include "util/Json.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

// The MCP server both fakes (stdio and HTTP) answer as: five tools covering read-only, destructive, plain, image and
// failing results.
namespace fake
{

// A 1x1 PNG.
constexpr const char* kPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

inline mcpchat::Json tools()
{
    using mcpchat::Json;
    auto tool = [](const char* name, const char* description, Json annotations, Json schema)
    {
        return Json{{"name", name}, {"description", description}, {"annotations", annotations}, {"inputSchema", schema}};
    };
    const Json numbers = {{"type", "object"},
                          {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}},
                          {"required", {"a", "b"}}};
    return Json::array({tool("add", "Adds two numbers.", {{"readOnlyHint", true}}, numbers),
                        tool("wipe", "Deletes everything.", {{"destructiveHint", true}}, {{"type", "object"}}),
                        tool("paint", "Paints.", {{"destructiveHint", false}}, {{"type", "object"}}),
                        tool("picture", "Returns a picture.", {{"readOnlyHint", true}}, {{"type", "object"}}),
                        tool("broken", "Always fails.", Json::object(), {{"type", "object"}})});
}

// The reply to one request, or nothing for a notification.
inline std::optional<mcpchat::Json> answer(const mcpchat::Json& request, std::vector<std::string>* calls = nullptr)
{
    using mcpchat::Json;
    if (!request.contains("id"))
        return std::nullopt;
    const std::string method = request.value("method", "");
    const Json params = request.value("params", Json::object());
    auto result = [&](Json value) { return Json{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", value}}; };
    auto error = [&](int code, const std::string& message)
    { return Json{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"error", {{"code", code}, {"message", message}}}}; };

    if (method == "initialize")
        return result({{"protocolVersion", params.value("protocolVersion", "2025-11-25")},
                       {"capabilities", {{"tools", Json::object()}}},
                       {"serverInfo", {{"name", "fake"}, {"title", "Fake Server"}, {"version", "1"}}},
                       {"instructions", "Use the tools."}});
    if (method == "ping")
        return result(Json::object());
    if (method == "tools/list")
        return result({{"tools", tools()}});
    if (method != "tools/call")
        return error(-32601, "Method not found: " + method);

    const std::string name = params.value("name", "");
    const Json arguments = params.value("arguments", Json::object());
    if (calls)
        calls->push_back(name);
    auto text = [](const std::string& value) { return Json::array({{{"type", "text"}, {"text", value}}}); };
    if (name == "add")
    {
        const double sum = arguments.value("a", 0.0) + arguments.value("b", 0.0);
        const Json structured = {{"sum", sum}};
        return result({{"content", text(mcpchat::dump(structured))}, {"structuredContent", structured}});
    }
    if (name == "wipe" || name == "paint")
        return result({{"content", text(name + " done")}});
    if (name == "picture")
        return result({{"content", Json::array({{{"type", "image"}, {"mimeType", "image/png"}, {"data", kPng}}})}});
    if (name == "broken")
        return result({{"content", text("it broke")}, {"isError", true}});
    return error(-32602, "Unknown tool: " + name);
}

} // namespace fake

#endif
