#include "Check.h"
#include "FakeLogic.h"
#include "LoopbackServer.h"

#include "agent/Agent.h"
#include "llm/AnthropicProvider.h"
#include "llm/ResponsesProvider.h"
#include "llm/OpenAiProvider.h"
#include "mcp/HttpTransport.h"
#include "mcp/ServerHub.h"
#include "util/Cancel.h"

#include <chrono>
#include <thread>

using namespace mcpchat;

namespace
{
// The fake MCP server over Streamable HTTP. `sse` answers requests as an event stream led by a notification;
// `token` requires that bearer token.
LoopbackServer::Handler mcpHandler(bool sse, const std::string& token = std::string())
{
    return [sse, token](const LoopbackServer::Request& request)
    {
        LoopbackServer::Reply reply;
        if (!token.empty() && (!request.headers.count("authorization") ||
                               request.headers.at("authorization") != "Bearer " + token))
        {
            reply.status = 401;
            reply.chunks = {"missing or wrong token"};
            return reply;
        }
        if (request.method == "DELETE")
            return reply;
        const Json message = Json::parse(request.body, nullptr, false);
        if (message.value("method", "") == "initialize")
            reply.headers.push_back({"Mcp-Session-Id", "session-7"});
        if (message.value("method", "") == "tools/call" && message["params"].value("name", "") == "reject")
        {
            reply.status = 400;
            reply.headers.push_back({"Content-Type", "application/json"});
            reply.chunks = {dump({{"jsonrpc", "2.0"},
                                  {"id", message["id"]},
                                  {"error", {{"code", -32602}, {"message", "bad arguments"}}}})};
            return reply;
        }
        const auto answer = fake::answer(message);
        if (!answer)
        {
            reply.status = 202;
            return reply;
        }
        if (sse)
        {
            reply.headers.push_back({"Content-Type", "text/event-stream"});
            reply.chunks = {"event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n\n",
                            "data: " + dump(*answer) + "\n\n"};
        }
        else
        {
            reply.headers.push_back({"Content-Type", "application/json"});
            reply.chunks = {dump(*answer)};
        }
        return reply;
    };
}

std::string sseChunk(const Json& delta, const std::string& finish = std::string())
{
    Json choice = {{"index", 0}, {"delta", delta}};
    if (!finish.empty())
        choice["finish_reason"] = finish;
    return "data: " + dump({{"choices", Json::array({choice})}}) + "\n\n";
}

// An LLM answering from a queue of replies; each reply is an event stream unless `json` is set.
struct FakeLlm
{
    std::vector<LoopbackServer::Reply> script;
    std::size_t next = 0;
    std::mutex mutex;

    LoopbackServer::Handler handler()
    {
        return [this](const LoopbackServer::Request&)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (next >= script.size())
            {
                LoopbackServer::Reply reply;
                reply.status = 500;
                reply.chunks = {R"({"error":{"message":"nothing scripted"}})"};
                return reply;
            }
            return script[next++];
        };
    }

    static LoopbackServer::Reply text(const std::string& value)
    {
        LoopbackServer::Reply reply;
        reply.headers.push_back({"Content-Type", "text/event-stream"});
        reply.chunks = {sseChunk({{"role", "assistant"}, {"content", value.substr(0, 3)}}),
                        sseChunk({{"content", value.substr(3)}}, "stop"), "data: [DONE]\n\n"};
        return reply;
    }

    static LoopbackServer::Reply call(const std::string& name, const std::string& arguments)
    {
        LoopbackServer::Reply reply;
        reply.headers.push_back({"Content-Type", "text/event-stream"});
        const std::size_t half = arguments.size() / 2;
        reply.chunks = {sseChunk({{"tool_calls", Json::array({{{"index", 0}, {"id", "c1"}, {"type", "function"},
                                                               {"function", {{"name", name}, {"arguments", arguments.substr(0, half)}}}}})}}),
                        sseChunk({{"tool_calls", Json::array({{{"index", 0}, {"function", {{"arguments", arguments.substr(half)}}}}})}},
                                 "tool_calls"),
                        "data: [DONE]\n\n"};
        return reply;
    }
};
} // namespace

TEST(httpTransportJsonAndSession)
{
    LoopbackServer server(mcpHandler(false));
    McpClient client(std::make_unique<HttpTransport>(server.url("/mcp"), std::vector<net::Header>{{"X-Extra", "1"}}, 10.0),
                     "tests", "1");
    client.initialize();
    CHECK(client.listTools().size() == 5);
    CHECK(client.callTool("add", {{"a", 1}, {"b", 2}}).structured["sum"] == 3.0);
    bool rejected = false;
    try
    {
        client.callTool("reject", Json::object());
    }
    catch (const McpError& error)
    {
        rejected = error.code == -32602 && error.message == "bad arguments";
    }
    CHECK(rejected);
    client.close();
    const std::vector<LoopbackServer::Request> requests = server.requests();
    CHECK(requests.size() == 6);
    CHECK(!requests[0].headers.count("mcp-session-id") && requests[0].headers.at("x-extra") == "1");
    CHECK(requests[1].headers.at("mcp-session-id") == "session-7");
    CHECK(requests[1].headers.at("mcp-protocol-version") == kLatestProtocolVersion);
    CHECK(Json::parse(requests[1].body)["method"] == "notifications/initialized");
    CHECK(requests[5].method == "DELETE" && requests[5].headers.at("mcp-session-id") == "session-7");
}

TEST(httpTransportEventStream)
{
    LoopbackServer server(mcpHandler(true));
    McpClient client(std::make_unique<HttpTransport>(server.url("/mcp"), std::vector<net::Header>(), 10.0), "tests", "1");
    client.initialize();
    CHECK(client.serverInfo()["title"] == "Fake Server");
    CHECK(client.callTool("broken", Json::object()).isError);
    CHECK(HttpTransport::answerFromEventStream("data: {\"id\":2,\"result\":{}}\n\n", Json(1)) == std::nullopt);
}

TEST(httpTransportTokenAndUnreachable)
{
    LoopbackServer server(mcpHandler(false, "t0ken"));
    ServerConfig config;
    config.name = "locked";
    config.url = server.url("/mcp");
    config.tokenEnv = "MCPCHAT_TEST_NO_SUCH_VARIABLE";
    SecretStore secrets;
    ServerHub denied({config}, &secrets);
    denied.connect();
    CHECK(denied.status()[0].error.find("401 (check the token)") != std::string::npos);
    secrets.setMemory(SecretStore::serverAccount("locked"), "t0ken");
    ServerHub allowed({config}, &secrets);
    allowed.connect();
    CHECK(allowed.status()[0].connected && allowed.tools().size() == 5);
}

TEST(providerStreamsTextAndToolCalls)
{
    FakeLlm llm;
    llm.script = {FakeLlm::call("add", R"({"a":2,"b":40})"), FakeLlm::text("The sum is 42.")};
    LoopbackServer server(llm.handler());
    OpenAiProvider::Settings settings;
    settings.baseUrl = server.url("/v1/");
    settings.model = "m";
    settings.apiKey = "secret";
    OpenAiProvider provider(settings);
    const AssistantMessage call = provider.complete({{{"role", "user"}, {"content", "hi"}}}, Json::array(), TextDelta(), nullptr);
    CHECK(call.toolCalls.size() == 1 && call.toolCalls[0].name == "add" && (*call.toolCalls[0].arguments)["b"] == 40);
    std::string streamed;
    const AssistantMessage text = provider.complete({{{"role", "user"}, {"content", "hi"}}}, Json::array(),
                                                    [&](const std::string& piece) { streamed += piece + "|"; }, nullptr);
    CHECK(text.content == "The sum is 42." && streamed == "The| sum is 42.|");
    const std::vector<LoopbackServer::Request> requests = server.requests();
    CHECK(requests[0].path == "/v1/chat/completions" && requests[0].headers.at("authorization") == "Bearer secret");
    const Json body = Json::parse(requests[0].body);
    CHECK(body["model"] == "m" && body["stream"] == true && !body.contains("tools"));
}

TEST(anthropicProviderStreamsTextAndToolCalls)
{
    const auto event = [](const std::string& type, Json payload)
    {
        payload["type"] = type;
        return "event: " + type + "\ndata: " + dump(payload) + "\n\n";
    };
    LoopbackServer::Reply reply;
    reply.headers.push_back({"Content-Type", "text/event-stream"});
    reply.chunks = {
        event("message_start", {{"message", {{"id", "m1"},
                                             {"role", "assistant"},
                                             {"usage", {{"input_tokens", 7}, {"output_tokens", 0}}}}}}),
        event("content_block_start", {{"index", 0}, {"content_block", {{"type", "text"}, {"text", ""}}}}),
        event("content_block_delta", {{"index", 0}, {"delta", {{"type", "text_delta"}, {"text", "The sum "}}}}),
        event("content_block_delta", {{"index", 0}, {"delta", {{"type", "text_delta"}, {"text", "is 42."}}}}),
        event("content_block_stop", {{"index", 0}}),
        event("content_block_start",
              {{"index", 1},
               {"content_block", {{"type", "tool_use"}, {"id", "toolu_1"}, {"name", "add"}, {"input", Json::object()}}}}),
        event("content_block_delta",
              {{"index", 1}, {"delta", {{"type", "input_json_delta"}, {"partial_json", R"({"a":2,)"}}}}),
        event("content_block_delta",
              {{"index", 1}, {"delta", {{"type", "input_json_delta"}, {"partial_json", R"("b":40})"}}}}),
        event("content_block_stop", {{"index", 1}}),
        event("message_delta", {{"delta", {{"stop_reason", "tool_use"}}}, {"usage", {{"output_tokens", 15}}}}),
        event("message_stop", Json::object()),
    };
    LoopbackServer server([&](const LoopbackServer::Request&) { return reply; });
    AnthropicProvider::Settings settings;
    settings.baseUrl = server.url("/v1/");
    settings.model = "claude-x";
    settings.apiKey = "key";
    AnthropicProvider provider(settings);
    std::string streamed;
    const AssistantMessage answer = provider.complete(
        {{{"role", "system"}, {"content", "be brief"}}, {{"role", "user"}, {"content", "hi"}}}, Json::array(),
        [&](const std::string& piece) { streamed += piece + "|"; }, nullptr);
    CHECK(answer.content == "The sum is 42." && streamed == "The sum |is 42.|");
    CHECK(answer.toolCalls.size() == 1 && answer.toolCalls[0].id == "toolu_1" && answer.toolCalls[0].name == "add");
    CHECK(answer.toolCalls[0].arguments && (*answer.toolCalls[0].arguments)["b"] == 40);
    CHECK(answer.finishReason == "tool_use");
    CHECK(answer.usage && answer.usage->promptTokens == 7 && answer.usage->completionTokens == 15);

    const LoopbackServer::Request request = server.requests().at(0);
    CHECK(request.path == "/v1/messages");
    CHECK(request.headers.count("x-api-key") == 1);
    CHECK(request.headers.at("anthropic-version") == "2023-06-01");
    const Json body = Json::parse(request.body);
    CHECK(body["model"] == "claude-x" && body["stream"] == true && body["max_tokens"] == 8192);
    CHECK(body["system"] == "be brief" && body["messages"][0]["role"] == "user");
}

TEST(anthropicProviderReadsAWholeMessageAndReportsErrors)
{
    LoopbackServer::Reply whole;
    whole.headers.push_back({"Content-Type", "application/json"});
    whole.chunks = {dump({{"id", "m1"},
                          {"role", "assistant"},
                          {"content", Json::array({{{"type", "text"}, {"text", "plain"}}})},
                          {"stop_reason", "end_turn"},
                          {"usage", {{"input_tokens", 3}, {"output_tokens", 1}}}})};
    LoopbackServer::Reply refused;
    refused.status = 401;
    refused.headers.push_back({"Content-Type", "application/json"});
    refused.chunks = {R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})"};
    std::vector<LoopbackServer::Reply> script = {whole, refused};
    std::size_t next = 0;
    LoopbackServer server(
        [&](const LoopbackServer::Request&)
        {
            return script.at(next++);
        });
    AnthropicProvider::Settings settings;
    // A bare host still gets the version prefix.
    settings.baseUrl = server.url("");
    settings.model = "claude-x";
    settings.stream = false;
    AnthropicProvider provider(settings);
    std::string streamed;
    const AssistantMessage answer = provider.complete(
        {{{"role", "user"}, {"content", "hi"}}}, Json::array(),
        [&](const std::string& piece) { streamed += piece; }, nullptr);
    CHECK(answer.content == "plain" && answer.toolCalls.empty() && answer.finishReason == "end_turn");
    CHECK(streamed == "plain");
    CHECK(server.requests().at(0).path == "/v1/messages");

    std::string message;
    try
    {
        provider.complete({{{"role", "user"}, {"content", "hi"}}}, Json::array(), TextDelta(), nullptr);
    }
    catch (const LlmError& error)
    {
        message = error.what();
    }
    CHECK(message == "The LLM server answered HTTP 401: invalid x-api-key");

    bool invalid = false;
    try
    {
        settings.baseUrl = "localhost:1234";
        AnthropicProvider broken(settings);
    }
    catch (const LlmError&)
    {
        invalid = true;
    }
    CHECK(invalid);
}

TEST(responsesProviderStreamsTextAndToolCalls)
{
    const auto event = [](const std::string& type, Json payload)
    {
        payload["type"] = type;
        return "event: " + type + "\ndata: " + dump(payload) + "\n\n";
    };
    LoopbackServer::Reply reply;
    reply.headers.push_back({"Content-Type", "text/event-stream"});
    reply.chunks = {
        event("response.created", {{"response", {{"status", "in_progress"}}}}),
        event("response.output_text.delta", {{"delta", "The sum "}}),
        event("response.output_text.delta", {{"delta", "is 42."}}),
        event("response.function_call_arguments.delta", {{"output_index", 1}, {"delta", R"({"a":2,)"}}),
        event("response.output_item.done",
              {{"item", {{"type", "function_call"},
                         {"call_id", "call_1"},
                         {"name", "add"},
                         {"arguments", R"({"a":2,"b":40})"}}}}),
        event("response.completed",
              {{"response", {{"status", "completed"}, {"usage", {{"input_tokens", 7}, {"output_tokens", 15}}}}}}),
    };
    LoopbackServer server([&](const LoopbackServer::Request&) { return reply; });
    ResponsesProvider::Settings settings;
    settings.baseUrl = server.url("/v1/");
    settings.model = "gpt-x";
    settings.apiKey = "key";
    settings.reasoningEffort = "medium";
    ResponsesProvider provider(settings);
    const Json tools = Json::array({{{"type", "function"},
                                     {"function", {{"name", "add"}, {"description", "adds"}, {"parameters", Json::object()}}}}});
    std::string streamed;
    const AssistantMessage answer = provider.complete(
        {{{"role", "system"}, {"content", "be brief"}},
         {{"role", "user"}, {"content", "hi"}},
         {{"role", "assistant"},
          {"content", ""},
          {"tool_calls", Json::array({{{"id", "c0"}, {"type", "function"}, {"function", {{"name", "add"}, {"arguments", "{}"}}}}})}},
         {{"role", "tool"}, {"tool_call_id", "c0"}, {"content", "0"}}},
        tools, [&](const std::string& piece) { streamed += piece + "|"; }, nullptr);
    CHECK(answer.content == "The sum is 42." && streamed == "The sum |is 42.|");
    CHECK(answer.toolCalls.size() == 1 && answer.toolCalls[0].id == "call_1" && answer.toolCalls[0].name == "add");
    CHECK(answer.toolCalls[0].arguments && (*answer.toolCalls[0].arguments)["b"] == 40);
    CHECK(answer.finishReason == "tool_calls");
    CHECK(answer.usage && answer.usage->promptTokens == 7 && answer.usage->completionTokens == 15);

    const LoopbackServer::Request request = server.requests().at(0);
    CHECK(request.path == "/v1/responses" && request.headers.at("authorization") == "Bearer key");
    const Json body = Json::parse(request.body);
    CHECK(body["model"] == "gpt-x" && body["stream"] == true && body["store"] == false);
    CHECK(body["instructions"] == "be brief" && body["reasoning"]["effort"] == "medium");
    CHECK(body["tools"][0]["name"] == "add" && body["tools"][0]["type"] == "function");
    CHECK(body["input"].size() == 3 && body["input"][0]["content"][0]["type"] == "input_text");
    CHECK(body["input"][1]["type"] == "function_call" && body["input"][1]["call_id"] == "c0");
    CHECK(body["input"][2]["type"] == "function_call_output" && body["input"][2]["output"] == "0");
}

TEST(responsesProviderReadsAWholeResponseAndReportsErrors)
{
    LoopbackServer::Reply whole;
    whole.headers.push_back({"Content-Type", "application/json"});
    whole.chunks = {dump({{"status", "completed"},
                          {"output", Json::array({{{"type", "reasoning"}, {"summary", Json::array()}},
                                                  {{"type", "message"},
                                                   {"content", Json::array({{{"type", "output_text"}, {"text", "plain"}}})}}})},
                          {"usage", {{"input_tokens", 3}, {"output_tokens", 1}}}})};
    LoopbackServer::Reply refused;
    refused.status = 400;
    refused.headers.push_back({"Content-Type", "application/json"});
    refused.chunks = {R"({"error":{"message":"Unsupported value"}})"};
    std::vector<LoopbackServer::Reply> script = {whole, refused};
    std::size_t next = 0;
    LoopbackServer server([&](const LoopbackServer::Request&) { return script.at(next++); });
    ResponsesProvider::Settings settings;
    settings.baseUrl = server.url("/v1");
    settings.model = "gpt-x";
    settings.stream = false;
    ResponsesProvider provider(settings);
    std::string streamed;
    const AssistantMessage answer = provider.complete({{{"role", "user"}, {"content", "hi"}}}, Json::array(),
                                                      [&](const std::string& piece) { streamed += piece; }, nullptr);
    CHECK(answer.content == "plain" && streamed == "plain" && answer.toolCalls.empty() && answer.finishReason == "stop");
    CHECK(!Json::parse(server.requests().at(0).body).contains("reasoning"));

    std::string message;
    try
    {
        provider.complete({{{"role", "user"}, {"content", "hi"}}}, Json::array(), TextDelta(), nullptr);
    }
    catch (const LlmError& error)
    {
        message = error.what();
    }
    CHECK(message == "The LLM server answered HTTP 400: Unsupported value");
}

TEST(providerErrorsAndFallbacks)
{
    FakeLlm llm;
    LoopbackServer::Reply refuse;
    refuse.status = 400;
    refuse.chunks = {R"({"error":{"message":"stream is not supported with tools"}})"};
    LoopbackServer::Reply whole;
    whole.headers.push_back({"Content-Type", "application/json"});
    whole.chunks = {R"({"choices":[{"message":{"content":"plain"},"finish_reason":"stop"}]})"};
    llm.script = {refuse, whole};
    LoopbackServer server(llm.handler());
    OpenAiProvider::Settings settings;
    settings.baseUrl = server.url("/v1");
    settings.model = "m";
    OpenAiProvider provider(settings);
    CHECK(provider.complete({{{"role", "user"}, {"content", "hi"}}}, Json::array(), TextDelta(), nullptr).content == "plain");
    CHECK(Json::parse(server.requests()[1].body)["stream"] == false);

    std::string message;
    try
    {
        provider.complete({{{"role", "user"}, {"content", "hi"}}}, Json::array(), TextDelta(), nullptr);
    }
    catch (const LlmError& error)
    {
        message = error.what();
    }
    CHECK(message == "The LLM server answered HTTP 500: nothing scripted");

    bool invalid = false;
    try
    {
        settings.baseUrl = "localhost:1234";
        OpenAiProvider broken(settings);
    }
    catch (const LlmError&)
    {
        invalid = true;
    }
    CHECK(invalid);

    settings.baseUrl = "http://127.0.0.1:9/v1";
    OpenAiProvider unreachable(settings);
    message.clear();
    try
    {
        unreachable.complete({{{"role", "user"}, {"content", "hi"}}}, Json::array(), TextDelta(), nullptr);
    }
    catch (const LlmError& error)
    {
        message = error.what();
    }
    CHECK(message.find("Connection to the LLM failed") == 0);
}

TEST(providerStopInterruptsAStream)
{
    FakeLlm llm;
    LoopbackServer::Reply slow;
    slow.headers.push_back({"Content-Type", "text/event-stream"});
    for (int i = 0; i < 20; ++i)
        slow.chunks.push_back(sseChunk({{"content", "."}}));
    slow.pauseMs = 300;
    llm.script = {slow};
    LoopbackServer server(llm.handler());
    OpenAiProvider::Settings settings;
    settings.baseUrl = server.url("/v1");
    settings.model = "m";
    OpenAiProvider provider(settings);
    CancelToken cancel;
    int pieces = 0;
    const auto start = std::chrono::steady_clock::now();
    bool cancelled = false;
    try
    {
        provider.complete({{{"role", "user"}, {"content", "hi"}}}, Json::array(),
                          [&](const std::string&)
                          {
                              if (++pieces == 2)
                                  cancel.cancel();
                          },
                          &cancel);
    }
    catch (const Cancelled&)
    {
        cancelled = true;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(cancelled && seconds < 3.0);
    server.stop();
}

TEST(agentEndToEndOverHttp)
{
    LoopbackServer mcp(mcpHandler(true));
    FakeLlm llm;
    llm.script = {FakeLlm::call("add", R"({"a":2,"b":40})"), FakeLlm::text("The sum is 42.")};
    LoopbackServer model(llm.handler());

    ServerConfig server;
    server.name = "fake";
    server.url = mcp.url("/mcp");
    SecretStore secrets;
    ServerHub hub({server}, &secrets);
    hub.connect();
    OpenAiProvider::Settings settings;
    settings.baseUrl = model.url("/v1");
    settings.model = "m";
    OpenAiProvider provider(settings);
    AgentListener listener;
    Agent agent(provider, hub, listener, AgentConfig(), Agent::Confirm());
    const RunResult result = agent.run("add 2 and 40", nullptr);
    CHECK(result.reason == RunReason::Done && result.steps == 2);
    const Json second = Json::parse(model.requests()[1].body);
    const Json& last = second["messages"].back();
    CHECK(last["role"] == "tool" && last["tool_call_id"] == "c1" &&
          last["content"].get<std::string>().find("\"sum\":42") != std::string::npos);
    CHECK(second["tools"].size() == 5 && second["messages"][0]["role"] == "system");
}
