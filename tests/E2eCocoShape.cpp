// mcpchat against a real CocoShape editor over MCP, with a scripted model.
// Runs when COCOSHAPE_BIN names a cocoshape binary (it needs a display, or xvfb-run); otherwise it is skipped.

#include "Check.h"
#include "LoopbackServer.h"

#include "agent/Agent.h"
#include "llm/Wire.h"
#include "mcp/Process.h"
#include "mcp/ServerHub.h"
#include "net/Http.h"
#include "util/Paths.h"

#include <chrono>
#include <cstdio>
#include <deque>
#include <thread>

using namespace mcpchat;

namespace
{
class ScriptedProvider : public LlmProvider
{
public:
    bool supportsImages() const override
    {
        return true;
    }

    AssistantMessage complete(const std::vector<Json>& messages, const Json&, const TextDelta&, CancelToken*) override
    {
        requests.push_back(messages);
        if (script.empty())
            throw LlmError("nothing scripted", 500);
        AssistantMessage reply = script.front();
        script.pop_front();
        return reply;
    }

    static AssistantMessage calls(const std::vector<std::pair<std::string, Json>>& wanted)
    {
        AssistantMessage reply;
        for (const auto& call : wanted)
        {
            ToolCall toolCall;
            toolCall.id = "call_" + std::to_string(++mNext);
            toolCall.name = call.first;
            toolCall.arguments = call.second;
            reply.toolCalls.push_back(toolCall);
        }
        return reply;
    }

    std::deque<AssistantMessage> script;
    std::vector<std::vector<Json>> requests;

private:
    static inline int mNext = 0;
};

struct Results : AgentListener
{
    void onToolResult(const std::string&, const std::string& name, const std::string& text, bool isError,
                      const std::vector<ToolImage>& images) override
    {
        if (isError)
            errors.push_back(name + ": " + text);
        for (const ToolImage& image : images)
            imageTypes.push_back(image.mimeType);
    }
    std::vector<std::string> errors;
    std::vector<std::string> imageTypes;
};

int freePort()
{
    LoopbackServer probe([](const LoopbackServer::Request&) { return LoopbackServer::Reply(); });
    const std::string url = probe.url("");
    probe.stop();
    return std::stoi(url.substr(url.rfind(':') + 1));
}

bool healthy(const std::string& base)
{
    net::Request request;
    request.method = "GET";
    request.url = base + "/api/health";
    request.connectTimeoutSeconds = 1.0;
    request.timeoutSeconds = 2.0;
    net::Response response;
    std::string error;
    return net::send(request, response, error) && response.status == 200;
}
} // namespace

TEST(aModelBuildsInTheEditor)
{
    const std::string binary = environment("COCOSHAPE_BIN");
    if (binary.empty())
    {
        std::printf("skipped: set COCOSHAPE_BIN to a cocoshape binary\n");
        return;
    }
    const int port = freePort();
    std::string error;
    std::unique_ptr<Process> editor = Process::start(binary, {"--api-port", std::to_string(port)}, {}, error);
    CHECK(editor != nullptr);
    if (!editor)
        return;
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    bool up = false;
    for (int i = 0; i < 120 && !up; ++i)
    {
        up = healthy(base);
        if (!up)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    CHECK(up);
    if (!up)
    {
        editor->stop(5.0);
        return;
    }

    ServerConfig server;
    server.name = "cocoshape";
    server.url = base + "/mcp";
    ServerHub hub({server}, nullptr);
    hub.connect();
    const std::vector<ServerStatus> status = hub.status();
    CHECK(status.size() == 1 && status[0].connected);
    CHECK(status[0].title.find("CocoShape") != std::string::npos);
    CHECK(hub.tools().count("add_primitive") == 1 && hub.tools().count("screenshot") == 1);

    ScriptedProvider model;
    model.script.push_back(ScriptedProvider::calls(
        {{"add_primitive", {{"type", "box"}, {"name", "body"}, {"size", {2, 1, 1}}, {"color", "#aa3300"}}},
         {"add_primitive",
          {{"type", "cylinder"}, {"name", "mast"}, {"radius", 0.1}, {"height", 2}, {"position", {0, 1.5, 0}}}}}));
    model.script.push_back(ScriptedProvider::calls({{"screenshot", {{"width", 160}, {"height", 120}}}}));
    model.script.push_back(ScriptedProvider::calls({{"bevel", {{"width", -1}}}}));
    AssistantMessage done;
    done.content = "Built a box with a mast.";
    model.script.push_back(done);

    Results results;
    AgentConfig config;
    config.confirm = ConfirmPolicy::Never;
    Agent agent(model, hub, results, config, nullptr);
    const RunResult run = agent.run("build a box with a mast", nullptr);
    CHECK(run.reason == RunReason::Done && run.steps == 4);

    const ToolResult parts = hub.call("get_status", Json::object(), nullptr);
    std::vector<std::string> names;
    for (const Json& block : parts.content)
    {
        if (block.value("type", "") != "text")
            continue;
        const Json status = Json::parse(block.value("text", "{}"), nullptr, false);
        if (status.is_object() && status.contains("parts"))
        {
            for (const Json& part : status["parts"])
                names.push_back(part.value("name", ""));
        }
    }
    CHECK((names == std::vector<std::string>{"body", "mast"}));

    CHECK(results.imageTypes == std::vector<std::string>{"image/png"});
    CHECK(results.errors.size() == 1 && results.errors[0].find("invalid_params") != std::string::npos);

    CHECK(model.requests.size() == 4);
    const std::string system = model.requests[0][0].value("content", "");
    CHECK(system.find("# CocoShape") != std::string::npos && system.find("Units are metres") != std::string::npos);
    // The screenshot reaches a model that can see as an image after the tool results.
    const Json wire = toWireMessages(model.requests[2], true);
    const Json& looked = wire.back();
    CHECK(looked.value("role", "") == "user" && looked["content"].is_array() &&
          looked["content"].size() >= 2 &&
          looked["content"][1]["image_url"]["url"].get<std::string>().rfind("data:image/png", 0) == 0);

    editor->stop(10.0);
}
