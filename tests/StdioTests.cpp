#include "Check.h"

#include "mcp/McpClient.h"
#include "mcp/ServerHub.h"
#include "mcp/StdioTransport.h"
#include "util/Cancel.h"

#include <chrono>
#include <thread>

using namespace mcpchat;

namespace
{
std::unique_ptr<McpClient> fakeClient(StdioTransport** raw = nullptr)
{
    auto transport = std::make_unique<StdioTransport>(FAKE_MCP_SERVER, std::vector<std::string>(),
                                                      std::map<std::string, std::string>{{"FAKE", "1"}}, 10.0);
    if (raw)
        *raw = transport.get();
    return std::make_unique<McpClient>(std::move(transport), "tests", "1");
}
} // namespace

TEST(stdioHandshakeToolsAndCalls)
{
    StdioTransport* transport = nullptr;
    auto client = fakeClient(&transport);
    client->initialize();
    CHECK(client->protocolVersion() == kLatestProtocolVersion);
    CHECK(client->serverInfo()["title"] == "Fake Server" && client->instructions() == "Use the tools.");
    CHECK(client->listTools().size() == 5);
    const ToolResult sum = client->callTool("add", {{"a", 2}, {"b", 3}});
    CHECK(!sum.isError && sum.structured["sum"] == 5.0);
    const ToolResult broken = client->callTool("broken", Json::object());
    CHECK(broken.isError && broken.text() == "it broke");
    const ToolResult picture = client->callTool("picture", Json::object());
    CHECK(picture.images().size() == 1 && picture.text().empty());
    bool unknown = false;
    try
    {
        client->callTool("nope", Json::object());
    }
    catch (const McpError& error)
    {
        unknown = error.code == -32602;
    }
    CHECK(unknown);
    // The server asks the client something in the middle of a call; the ping is answered.
    CHECK(client->callTool("ask", Json::object()).text() == "pong received");
    CHECK(transport->stderrTail().find("fake server ready") != std::string::npos);
    client->close();
}

TEST(stdioServerThatDiesIsReported)
{
    auto client = fakeClient();
    client->initialize();
    std::string message;
    try
    {
        client->callTool("crash", Json::object());
    }
    catch (const TransportError& error)
    {
        message = error.what();
    }
    CHECK(message.find("exited with code 3") != std::string::npos || message.find("closed its output") != std::string::npos);
    CHECK(message.find("about to crash") != std::string::npos);

    // Windows hands an unknown command to cmd.exe, which starts and then fails: either way the handshake cannot work.
    bool failed = false;
    try
    {
        McpClient missing(std::make_unique<StdioTransport>("mcpchat-no-such-program", std::vector<std::string>(),
                                                           std::map<std::string, std::string>(), 5.0),
                          "tests", "1");
        missing.initialize();
    }
    catch (const TransportError&)
    {
        failed = true;
    }
    CHECK(failed);
}

TEST(stdioCancelWakesAWaitingCall)
{
    auto client = fakeClient();
    client->initialize();
    CancelToken cancel;
    std::thread stopper([&]
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                            cancel.cancel();
                        });
    const auto start = std::chrono::steady_clock::now();
    bool cancelled = false;
    try
    {
        client->callTool("sleep", Json::object(), &cancel);
    }
    catch (const Cancelled&)
    {
        cancelled = true;
    }
    stopper.join();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(cancelled && seconds < 3.0);
}

TEST(hubPrefixesSeveralServersAndReportsDeadOnes)
{
    std::vector<ServerConfig> servers(4);
    servers[0].name = "one";
    servers[0].command = FAKE_MCP_SERVER;
    servers[1].name = "two";
    servers[1].command = FAKE_MCP_SERVER;
    servers[2].name = "gone";
    servers[2].url = "http://127.0.0.1:9/mcp";
    servers[2].timeout = 2.0;
    servers[3].name = "off";
    servers[3].command = FAKE_MCP_SERVER;
    servers[3].enabled = false;
    SecretStore secrets;
    ServerHub hub(servers, &secrets);
    hub.connect();
    CHECK(hub.tools().size() == 10 && hub.tools().count("one__add") && hub.tools().count("two__picture"));
    CHECK(hub.call("two__add", {{"a", 2}, {"b", 3}}, nullptr).structured["sum"] == 5.0);
    const std::vector<ServerStatus> status = hub.status();
    CHECK(status.size() == 3);
    CHECK(status[0].connected && status[0].tools.size() == 5 && status[0].title == "Fake Server");
    CHECK(!status[2].connected && status[2].error.find("cannot reach") != std::string::npos);
    CHECK(hub.instructions().size() == 2);
    hub.close();

    ServerHub single({servers[0]}, &secrets);
    single.connect();
    CHECK(single.tools().count("add") && single.tools().at("add").readOnly() && single.tools().at("wipe").destructive() &&
          !single.tools().at("paint").destructive());
}
