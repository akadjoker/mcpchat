#include "Check.h"

#include "agent/History.h"
#include "agent/Tools.h"
#include "config/Config.h"
#include "llm/Wire.h"
#include "mcp/ServerHub.h"
#include "net/Sse.h"
#include "net/Url.h"
#include "ui/TextWrap.h"
#include "util/Base64.h"
#include "util/Strings.h"

#include <filesystem>

using namespace mcpchat;

TEST(base64RoundTrip)
{
    const std::string text = "MCP chat \xC3\xA7 \x01\x02\xFF";
    const std::string encoded = base64Encode(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    std::vector<std::uint8_t> decoded;
    CHECK(base64Decode(encoded, decoded));
    CHECK(std::string(decoded.begin(), decoded.end()) == text);
    CHECK(base64Encode(reinterpret_cast<const std::uint8_t*>("ab"), 2) == "YWI=");
    CHECK(!base64Decode("Y$I=", decoded));
    CHECK(base64Decode("YW\nI=", decoded) && decoded.size() == 2);
}

TEST(stringsCutWholeCharacters)
{
    const std::string text = "a\xC3\xA7\xC3\xA3o";
    CHECK(utf8Prefix(text, 2) == "a");
    CHECK(utf8Prefix(text, 3) == "a\xC3\xA7");
    CHECK(truncate("abcdef", 3) == "abc\n[truncated: 3 more bytes]");
    CHECK(truncate("abc", 3) == "abc");
    CHECK(trim("  x y \n") == "x y");
}

TEST(wrapBreaksAtSpacesInsideWordsAndNewlines)
{
    // Every character is one unit wide; "\xC3\xA7" is one character.
    const auto measure = [](std::string_view text)
    {
        float width = 0.0f;
        for (const char c : text)
            width += (static_cast<unsigned char>(c) & 0xC0) == 0x80 ? 0.0f : 1.0f;
        return width;
    };
    std::vector<std::string> lines = wrapText("the quick brown fox", 10.0f, measure);
    CHECK(lines.size() == 2 && lines[0] == "the quick" && lines[1] == "brown fox");
    lines = wrapText("abcdefghij", 4.0f, measure);
    CHECK(lines.size() == 3 && lines[0] == "abcd" && lines[2] == "ij");
    lines = wrapText("a\xC3\xA7\xC3\xA7\xC3\xA7" "b", 2.0f, measure);
    CHECK(lines.size() == 3 && lines[0] == "a\xC3\xA7" && lines[1] == "\xC3\xA7\xC3\xA7" && lines[2] == "b");
    lines = wrapText("one\r\n\ntwo", 50.0f, measure);
    CHECK(lines.size() == 3 && lines[0] == "one" && lines[1].empty() && lines[2] == "two");
    lines = wrapText("xyz", 0.5f, measure);
    CHECK(lines.size() == 3 && lines[0] == "x");
    CHECK(wrapText("", 10.0f, measure).size() == 1);
}

TEST(urlParsing)
{
    net::Url url;
    CHECK(net::Url::parse("http://localhost:11434/v1", url) && url.host == "localhost" && url.port == 11434 &&
          url.path == "/v1");
    CHECK(net::Url::parse("https://api.deepseek.com", url) && url.port == 443 && url.path == "/");
    CHECK(net::Url::parse("http://[::1]:7420/mcp?x=1", url) && url.host == "::1" && url.port == 7420 &&
          url.path == "/mcp?x=1" && url.authority() == "[::1]:7420");
    CHECK(!net::Url::parse("ftp://host/file", url));
    CHECK(!net::Url::parse("localhost:11434", url));
}

TEST(sseInPieces)
{
    const std::string stream = ": comment\r\nevent: message\r\ndata: {\"a\":1}\r\n\r\ndata: line one\ndata: line two\n\n"
                               "id: 7\ndata: last";
    std::vector<std::string> events;
    net::SseParser parser([&](const std::string& data)
                          {
                              events.push_back(data);
                              return true;
                          });
    for (const char c : stream)
        CHECK(parser.feed(std::string_view(&c, 1)));
    CHECK(parser.finish());
    CHECK(events.size() == 3 && events[0] == "{\"a\":1}" && events[1] == "line one\nline two" && events[2] == "last");

    int seen = 0;
    net::SseParser stopping([&](const std::string&) { return ++seen < 1; });
    CHECK(!stopping.feed("data: 1\n\ndata: 2\n\n") && seen == 1);
}

TEST(configRoundTripAndRefusals)
{
    Config config = Config::example();
    config.profiles[0].temperature = 0.25;
    config.servers.push_back(ServerConfig());
    config.servers.back().name = "local";
    config.servers.back().command = "python3";
    config.servers.back().args = {"server.py", "--x"};
    config.servers.back().env = {{"KEY", "value"}};
    config.confirm = ConfirmPolicy::Writes;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "mcpchat_test" / "config.json";
    std::string error;
    CHECK(saveConfig(path, config, error));
    Config read;
    CHECK(loadConfig(path, read, error));
    CHECK(read.servers.size() == 2 && read.servers[1].args.size() == 2 && read.servers[1].env.at("KEY") == "value");
    CHECK(read.profiles.size() == 3 && read.profiles[0].temperature && *read.profiles[0].temperature == 0.25);
    CHECK(!read.profiles[1].temperature);
    CHECK(read.confirm == ConfirmPolicy::Writes && read.profile()->name == "ollama");
    CHECK(read.problems().empty());

    Config bad;
    CHECK(!fromJson(Json::parse(R"({"servers":[{"name":"x","url":"http://h","colour":1}]})"), bad, error) &&
          error.find("unknown field(s) colour") != std::string::npos);
    CHECK(!fromJson(Json::parse(R"({"profiles":[{"name":"x","vision":"yes"}]})"), bad, error) &&
          error.find("'vision' must be true or false") != std::string::npos);
    CHECK(!fromJson(Json::parse(R"({"confirm":"always"})"), bad, error));

    Config twins = Config::example();
    twins.profiles.push_back(twins.profiles[0]);
    twins.servers.push_back(ServerConfig());
    const std::vector<std::string> problems = twins.problems();
    bool duplicate = false, noName = false;
    for (const std::string& problem : problems)
    {
        duplicate = duplicate || problem == "two profiles are called 'ollama'";
        noName = noName || problem == "a server has no name";
    }
    CHECK(duplicate && noName);

    Config missing;
    CHECK(loadConfig(path.parent_path() / "absent.json", missing, error) && missing.profiles.empty());
    std::filesystem::remove_all(path.parent_path());
}

TEST(schemasForPickyServers)
{
    const Json schema = Json::parse(R"({
        "type": "object", "additionalProperties": false, "$schema": "x",
        "properties": {
            "size": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3, "description": "XYZ."},
            "part": {"description": "Which part.", "oneOf": [{"type": "integer"}, {"type": "string"}]},
            "mode": {"anyOf": [{"enum": ["a", "b"]}, {"type": "array", "items": {"type": "string"}, "minItems": 1}]}
        }})");
    const Json simple = simplifySchema(schema);
    CHECK(!simple.contains("additionalProperties") && !simple.contains("$schema"));
    CHECK(simple["properties"]["size"]["description"] == "XYZ. Exactly 3 numbers.");
    CHECK(!simple["properties"]["size"].contains("minItems"));
    CHECK(simple["properties"]["part"]["description"] == "Which part. Accepts an integer or a string.");
    CHECK(simple["properties"]["mode"]["description"] ==
          "Accepts one of \"a\", \"b\" or an array of at least 1 strings.");

    const Json tool = functionTool("add", {{"name", "add"}, {"title", "Add"}}, false);
    CHECK(tool["function"]["description"] == "Add");
    CHECK(tool["function"]["parameters"] == Json({{"type", "object"}, {"properties", Json::object()}}));
}

TEST(exposedNamesFitFunctionNameRules)
{
    CHECK(ServerHub::exposedName("my server", "do.thing", true) == "my_server__do_thing");
    CHECK(ServerHub::exposedName("x", "tool", false) == "tool");
    const std::string longName = ServerHub::exposedName(std::string(40, 's'), std::string(40, 't'), true);
    CHECK(longName.size() == 64);
    CHECK(longName != ServerHub::exposedName(std::string(40, 's'), std::string(39, 't') + "u", true));
}

TEST(wireMessagesCarryImagesAfterToolRuns)
{
    const std::vector<Json> messages = {
        {{"role", "user"}, {"content", "look"}},
        Json::parse(R"({"role":"assistant","content":"","tool_calls":[{"id":"1","type":"function",
            "function":{"name":"picture","arguments":"{}"}},{"id":"2","type":"function",
            "function":{"name":"picture","arguments":"{}"}}]})"),
        {{"role", "tool"}, {"tool_call_id", "1"}, {"content", "a"}, {"image", {{"mimeType", "image/png"}, {"data", "AA"}}}},
        {{"role", "tool"}, {"tool_call_id", "2"}, {"content", "b"}, {"image", {{"mimeType", "image/jpeg"}, {"data", "BB"}}}},
        {{"role", "user"}, {"content", "and?"}}};
    const Json seeing = toWireMessages(messages, true);
    CHECK(seeing.size() == 6);
    CHECK(seeing[1]["content"].is_null());
    CHECK(seeing[2]["role"] == "tool" && !seeing[2].contains("image"));
    CHECK(seeing[4]["role"] == "user" && seeing[4]["content"][0]["text"] == "Images returned by the tool calls above.");
    CHECK(seeing[4]["content"][2]["image_url"]["url"] == "data:image/jpeg;base64,BB");
    CHECK(seeing[5]["content"] == "and?");
    CHECK(toWireMessages(messages, false).size() == 5);
}

TEST(replyBuilderJoinsStreamFragments)
{
    ReplyBuilder builder;
    builder.addChunk(Json::parse(R"({"choices":[{"delta":{"content":"Hel"}}]})"));
    builder.addChunk(Json::parse(R"({"choices":[{"delta":{"content":"lo","tool_calls":[{"index":0,"id":"c1",
        "function":{"name":"add","arguments":"{\"a\":"}}]}}]})"));
    builder.addChunk(Json::parse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"1}"}},
        {"index":1,"id":"c1","function":{"name":"wipe","arguments":""}}]}}]})"));
    builder.addChunk(Json::parse(R"({"choices":[{"delta":{"tool_calls":[{"index":2,"function":{"name":"x",
        "arguments":"[1]"}}]},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":5,"completion_tokens":7}})"));
    const AssistantMessage reply = builder.build();
    CHECK(reply.content == "Hello" && reply.finishReason == "tool_calls");
    CHECK(reply.usage && reply.usage->promptTokens == 5 && reply.usage->completionTokens == 7);
    CHECK(reply.toolCalls.size() == 3);
    CHECK(reply.toolCalls[0].arguments && (*reply.toolCalls[0].arguments)["a"] == 1);
    CHECK(reply.toolCalls[1].id == "c1_" && reply.toolCalls[1].arguments == Json::object());
    CHECK(reply.toolCalls[2].id == "call_2" && !reply.toolCalls[2].arguments &&
          reply.toolCalls[2].error == "the arguments must be a JSON object");

    const Json history = assistantToHistory(reply);
    CHECK(history["tool_calls"][2]["function"]["arguments"] == "{}");

    ReplyBuilder whole;
    CHECK(whole.addMessage(Json::parse(R"({"choices":[{"message":{"content":"ok","tool_calls":[
        {"id":"x","type":"function","function":{"name":"add","arguments":"{\"a\":2}"}}]},"finish_reason":"stop"}]})")) ==
          "ok");
    CHECK(whole.build().toolCalls.size() == 1 && whole.build().toolCalls[0].name == "add");
    CHECK(parseToolCall("1", "", "{}").error == "the tool call has no function name");
    CHECK(parseToolCall("1", "f", "{oops").error == "the arguments are not valid JSON");
}

TEST(historyPruning)
{
    std::vector<Json> messages;
    messages.push_back({{"role", "user"}, {"content", "first"}});
    for (int i = 0; i < 4; ++i)
        messages.push_back({{"role", "tool"},
                            {"tool_call_id", std::to_string(i)},
                            {"content", std::string(1000, 'x')},
                            {"image", {{"mimeType", "image/png"}, {"data", "AA"}}}});
    messages.push_back({{"role", "user"}, {"content", "second"}});
    PruneLimits limits;
    limits.keepImages = 2;
    limits.recentResults = 3;
    limits.oldResultChars = 100;
    limits.maxChars = 1000000;
    pruneHistory(messages, limits);
    CHECK(!messages[1].contains("image") && !messages[2].contains("image") && messages[3].contains("image"));
    CHECK(messages[1]["content"].get<std::string>().find("[...omitted from history]") != std::string::npos);
    CHECK(messages[2]["content"].get<std::string>().size() == 1000 + std::string("\n[screenshot no longer attached]").size());

    limits.maxChars = 10;
    pruneHistory(messages, limits);
    CHECK(messages.size() == 1 && messages[0]["content"] == "second");
}
