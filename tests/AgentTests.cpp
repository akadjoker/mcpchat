#include "Check.h"
#include "FakeLogic.h"

#include "agent/Agent.h"
#include "util/Cancel.h"

#include <deque>

using namespace mcpchat;

namespace
{
// Replies scripted in advance; records what each request carried.
class ScriptedProvider : public LlmProvider
{
public:
    explicit ScriptedProvider(bool vision = false) : mVision(vision)
    {
    }

    bool supportsImages() const override
    {
        return mVision;
    }

    AssistantMessage complete(const std::vector<Json>& messages, const Json& tools, const TextDelta& onText,
                              CancelToken* cancel) override
    {
        requests.push_back(messages);
        toolLists.push_back(tools);
        if (cancelOnRequest == static_cast<int>(requests.size()) && cancel)
            cancel->cancel();
        if (cancel && cancel->isSet())
            throw Cancelled();
        if (script.empty())
            throw LlmError("nothing scripted", 500);
        AssistantMessage reply = script.front();
        script.pop_front();
        if (!reply.content.empty() && onText)
            onText(reply.content);
        return reply;
    }

    static AssistantMessage text(const std::string& value)
    {
        AssistantMessage reply;
        reply.content = value;
        return reply;
    }

    static AssistantMessage calls(const std::vector<std::pair<std::string, Json>>& wanted)
    {
        AssistantMessage reply;
        int id = 0;
        for (const auto& call : wanted)
        {
            ToolCall toolCall;
            toolCall.id = "call_" + std::to_string(id++);
            toolCall.name = call.first;
            toolCall.arguments = call.second;
            reply.toolCalls.push_back(toolCall);
        }
        return reply;
    }

    std::deque<AssistantMessage> script;
    std::vector<std::vector<Json>> requests;
    std::vector<Json> toolLists;
    int cancelOnRequest = 0;

private:
    bool mVision;
};

// The fake server's tools, answered in-process; `unreachable` makes every call a transport failure.
class FakeHost : public ToolHost
{
public:
    FakeHost()
    {
        for (const Json& tool : fake::tools())
        {
            const std::string name = tool["name"].get<std::string>();
            mTools[name] = ExposedTool{name, "fake", tool};
        }
    }

    const std::map<std::string, ExposedTool>& tools() const override
    {
        return mTools;
    }

    std::vector<std::pair<std::string, std::string>> instructions() const override
    {
        return {{"Fake Server", "Use the tools."}};
    }

    ToolResult call(const std::string& name, const Json& arguments, CancelToken*) override
    {
        if (unreachable)
            throw TransportError("cannot reach http://127.0.0.1:9/mcp: connection refused");
        const auto reply = fake::answer(
            {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", name}, {"arguments", arguments}}}},
            &calls);
        if ((*reply).contains("error"))
            throw McpError((*reply)["error"]["code"].get<int>(), (*reply)["error"]["message"].get<std::string>());
        ToolResult result;
        result.content = (*reply)["result"]["content"];
        result.isError = (*reply)["result"].value("isError", false);
        if ((*reply)["result"].contains("structuredContent"))
            result.structured = (*reply)["result"]["structuredContent"];
        return result;
    }

    std::vector<std::string> calls;
    bool unreachable = false;

private:
    std::map<std::string, ExposedTool> mTools;
};

class Recorder : public AgentListener
{
public:
    void onTextDelta(const std::string& value) override
    {
        text += value;
    }
    void onToolCall(const std::string&, const std::string& name, const Json& arguments) override
    {
        calls.push_back({name, arguments});
    }
    void onToolResult(const std::string&, const std::string& name, const std::string& value, bool isError,
                      const std::vector<ToolImage>& images) override
    {
        results.push_back({name, value, isError, images.size()});
        if (!images.empty())
            firstImageBytes = images[0].bytes.size();
    }
    void onError(const std::string& message) override
    {
        errors.push_back(message);
    }

    struct Result
    {
        std::string name;
        std::string text;
        bool isError;
        std::size_t images;
    };
    std::string text;
    std::vector<std::pair<std::string, Json>> calls;
    std::vector<Result> results;
    std::vector<std::string> errors;
    std::size_t firstImageBytes = 0;
};
} // namespace

TEST(agentToolRoundTripAndFinalAnswer)
{
    ScriptedProvider provider;
    provider.script = {ScriptedProvider::calls({{"add", {{"a", 2}, {"b", 40}}}}), ScriptedProvider::text("The sum is 42.")};
    FakeHost host;
    Recorder recorder;
    Agent agent(provider, host, recorder, AgentConfig(), Agent::Confirm());
    const RunResult result = agent.run("add 2 and 40", nullptr);
    CHECK(result.reason == RunReason::Done && result.steps == 2);
    CHECK(recorder.calls.size() == 1 && recorder.calls[0].first == "add" && recorder.calls[0].second["b"] == 40);
    CHECK(recorder.text == "The sum is 42.");
    CHECK(host.calls == std::vector<std::string>({"add"}));
    CHECK(provider.toolLists[0].size() == 5);
    CHECK(provider.requests[0][0]["content"].get<std::string>().find("# Fake Server\nUse the tools.") != std::string::npos);
    const Json& last = provider.requests[1].back();
    CHECK(last["role"] == "tool" && last["content"].get<std::string>().find("\"sum\":42") != std::string::npos);
    CHECK(agent.messages().size() == 4);
}

TEST(agentToolErrorsReachTheModel)
{
    ScriptedProvider provider;
    provider.script = {ScriptedProvider::calls({{"broken", Json::object()}, {"nope", Json::object()}}),
                       ScriptedProvider::text("Could not do it.")};
    ToolCall invalid;
    invalid.id = "bad";
    invalid.name = "add";
    invalid.error = "the arguments are not valid JSON";
    provider.script[0].toolCalls.push_back(invalid);
    FakeHost host;
    Recorder recorder;
    Agent agent(provider, host, recorder, AgentConfig(), Agent::Confirm());
    CHECK(agent.run("try", nullptr).reason == RunReason::Done);
    CHECK(recorder.results.size() == 3);
    CHECK(recorder.results[0].text == "it broke" && recorder.results[0].isError);
    CHECK(recorder.results[1].isError && recorder.results[1].text.find("there is no tool 'nope'") != std::string::npos);
    CHECK(recorder.results[2].text.find("Send the arguments as one JSON object") != std::string::npos);
    int toolMessages = 0;
    for (const Json& message : provider.requests[1])
        toolMessages += message["role"] == "tool" ? 1 : 0;
    CHECK(toolMessages == 3);
}

TEST(agentAsksBeforeDestructiveTools)
{
    std::vector<std::string> asked;
    ScriptedProvider provider;
    provider.script = {ScriptedProvider::calls({{"wipe", Json::object()}, {"paint", Json::object()}}),
                       ScriptedProvider::text("ok")};
    FakeHost host;
    Recorder recorder;
    Agent agent(provider, host, recorder, AgentConfig(),
                [&](const std::string& name, const Json&)
                {
                    asked.push_back(name);
                    return false;
                });
    agent.run("clean up", nullptr);
    CHECK(asked == std::vector<std::string>({"wipe"}));
    CHECK(host.calls == std::vector<std::string>({"paint"}));
    CHECK(recorder.results[0].text.find("declined") != std::string::npos);

    ScriptedProvider writes;
    writes.script = {ScriptedProvider::calls({{"paint", Json::object()}, {"add", {{"a", 1}, {"b", 1}}}}),
                     ScriptedProvider::text("ok")};
    AgentConfig config;
    config.confirm = ConfirmPolicy::Writes;
    Agent strict(writes, host, recorder, config,
                 [&](const std::string& name, const Json&)
                 {
                     asked.push_back(name);
                     return true;
                 });
    strict.run("paint", nullptr);
    CHECK(asked.back() == "paint" && asked.size() == 2);

    config.confirm = ConfirmPolicy::Never;
    ScriptedProvider never;
    never.script = {ScriptedProvider::calls({{"wipe", Json::object()}}), ScriptedProvider::text("ok")};
    Agent trusting(never, host, recorder, config,
                   [&](const std::string& name, const Json&)
                   {
                       asked.push_back(name);
                       return false;
                   });
    trusting.run("wipe", nullptr);
    CHECK(asked.size() == 2 && host.calls.back() == "wipe");
}

TEST(agentImagesGoToAModelThatCanSee)
{
    ScriptedProvider seeing(true);
    seeing.script = {ScriptedProvider::calls({{"picture", Json::object()}}), ScriptedProvider::text("I see it.")};
    FakeHost host;
    Recorder recorder;
    Agent agent(seeing, host, recorder, AgentConfig(), Agent::Confirm());
    agent.run("look", nullptr);
    CHECK(recorder.results[0].images == 1 && recorder.firstImageBytes > 60);
    CHECK(seeing.requests[1].back()["image"]["data"] == fake::kPng);

    ScriptedProvider blind(false);
    blind.script = {ScriptedProvider::calls({{"picture", Json::object()}}), ScriptedProvider::text("ok")};
    Agent blindAgent(blind, host, recorder, AgentConfig(), Agent::Confirm());
    blindAgent.run("look", nullptr);
    const Json& last = blind.requests[1].back();
    CHECK(!last.contains("image") && last["content"].get<std::string>().find("cannot see images") != std::string::npos);
    CHECK(blindAgent.exportConversation()["messages"].size() == 4);
    CHECK(agent.exportConversation()["messages"][2]["image"]["data"] == "<omitted>");
}

TEST(agentStopsWhenTheServerIsUnreachable)
{
    ScriptedProvider provider;
    provider.script = {ScriptedProvider::calls({{"add", {{"a", 1}, {"b", 1}}}, {"add", {{"a", 2}, {"b", 2}}}})};
    FakeHost host;
    host.unreachable = true;
    Recorder recorder;
    Agent agent(provider, host, recorder, AgentConfig(), Agent::Confirm());
    const RunResult result = agent.run("add", nullptr);
    CHECK(result.reason == RunReason::Error);
    CHECK(!recorder.errors.empty() && recorder.errors.back().find("cannot reach") != std::string::npos);
    const std::vector<Json>& messages = agent.messages();
    CHECK(messages.size() == 4 && messages[2]["role"] == "tool" && messages[3]["role"] == "tool");
    CHECK(messages[3]["content"] == "Not run: the server is unreachable.");
}

TEST(agentReportsLlmErrorsAndStepLimits)
{
    ScriptedProvider provider;
    FakeHost host;
    Recorder recorder;
    Agent agent(provider, host, recorder, AgentConfig(), Agent::Confirm());
    CHECK(agent.run("hi", nullptr).reason == RunReason::Error && recorder.errors[0] == "nothing scripted");

    ScriptedProvider looping;
    for (int i = 0; i < 3; ++i)
        looping.script.push_back(ScriptedProvider::calls({{"add", {{"a", i}, {"b", 1}}}}));
    AgentConfig config;
    config.maxSteps = 3;
    Agent limited(looping, host, recorder, config, Agent::Confirm());
    const RunResult result = limited.run("loop", nullptr);
    CHECK(result.reason == RunReason::MaxSteps && result.steps == 3);
    CHECK(recorder.errors.back().find("Stopped after 3 steps") != std::string::npos);

    ScriptedProvider empty;
    empty.script = {ScriptedProvider::text("  ")};
    Agent silent(empty, host, recorder, AgentConfig(), Agent::Confirm());
    CHECK(silent.run("hi", nullptr).reason == RunReason::Done && recorder.errors.back() == "The model returned an empty answer.");
}

TEST(agentStopsOnCancel)
{
    ScriptedProvider provider;
    provider.script = {ScriptedProvider::calls({{"add", {{"a", 1}, {"b", 1}}}})};
    provider.cancelOnRequest = 1;
    FakeHost host;
    Recorder recorder;
    Agent agent(provider, host, recorder, AgentConfig(), Agent::Confirm());
    CancelToken cancel;
    CHECK(agent.run("add", &cancel).reason == RunReason::Cancelled);
    CHECK(host.calls.empty());
}
