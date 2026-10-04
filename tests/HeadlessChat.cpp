// mcpchat without the window: one message through the real configuration, model and MCP servers, printed to the
// terminal. For trying a profile or a server from a script, where nobody can click.
//
//   mcpchat_headless [--config <file>] [--profile <name>] [--attach <image>] <message>

#include "agent/Agent.h"
#include "config/Config.h"
#include "llm/AnthropicProvider.h"
#include "llm/OpenAiProvider.h"
#include "llm/Wire.h"
#include "mcp/ServerHub.h"
#include "util/Base64.h"
#include "util/Paths.h"
#include "util/Strings.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace mcpchat;

namespace
{
double seconds()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

class Printer : public AgentListener
{
public:
    void onStep(int step, int maxSteps) override
    {
        std::printf("\n[step %d of %d, %.0f s]\n", step, maxSteps, seconds());
        std::fflush(stdout);
    }
    void onTextDelta(const std::string& text) override
    {
        std::fputs(text.c_str(), stdout);
        std::fflush(stdout);
    }
    void onToolCall(const std::string&, const std::string& name, const Json& arguments) override
    {
        std::printf("\n[tool %s, %.2f MB of arguments]\n", name.c_str(), static_cast<double>(dump(arguments).size()) / 1.0e6);
        std::fflush(stdout);
    }
    void onToolResult(const std::string&, const std::string& name, const std::string& text, bool isError,
                      const std::vector<ToolImage>& images) override
    {
        std::printf("[%s %s: %s%s]\n", name.c_str(), isError ? "FAILED" : "answered",
                    std::string(utf8Prefix(text, 240)).c_str(), images.empty() ? "" : " (+image)");
        std::fflush(stdout);
        failures += isError ? 1 : 0;
    }
    void onError(const std::string& message) override
    {
        std::printf("\n[error: %s]\n", message.c_str());
        std::fflush(stdout);
        ++failures;
    }

    int failures = 0;
};

std::string mimeOf(const std::filesystem::path& path)
{
    const std::string extension = toLower(path.extension().u8string());
    if (extension == ".png")
        return "image/png";
    if (extension == ".gif")
        return "image/gif";
    if (extension == ".bmp")
        return "image/bmp";
    return "image/jpeg";
}
} // namespace

int main(int argc, char** argv)
{
    std::filesystem::path configPath = defaultConfigPath();
    std::string profileName;
    std::string attach;
    std::string message;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--config" && i + 1 < argc)
            configPath = argv[++i];
        else if (argument == "--profile" && i + 1 < argc)
            profileName = argv[++i];
        else if (argument == "--attach" && i + 1 < argc)
            attach = argv[++i];
        else
            message += (message.empty() ? "" : " ") + argument;
    }
    if (message.empty() && attach.empty())
    {
        std::fprintf(stderr, "usage: mcpchat_headless [--config <file>] [--profile <name>] [--attach <image>] <message>\n");
        return 2;
    }

    Config config;
    std::string error;
    if (!loadConfig(configPath, config, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    if (!profileName.empty())
        config.currentProfile = profileName;
    const Profile* profile = config.profile();
    if (!profile || (!profileName.empty() && profile->name != profileName))
    {
        std::fprintf(stderr, "no such profile\n");
        return 2;
    }
    std::printf("profile %s (%s), confirm %s\n", profile->name.c_str(), profile->model.c_str(), toString(config.confirm));

    std::vector<UserImage> images;
    if (!attach.empty())
    {
        std::ifstream file(attach, std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.empty())
        {
            std::fprintf(stderr, "cannot read %s\n", attach.c_str());
            return 2;
        }
        std::error_code code;
        images.push_back({mimeOf(attach), base64Encode(bytes.data(), bytes.size()),
                          std::filesystem::absolute(attach, code).lexically_normal().u8string()});
    }

    ServerHub hub(config.servers, nullptr);
    hub.connect();
    for (const ServerStatus& status : hub.status())
        std::printf("server %s: %s\n", status.name.c_str(),
                    status.connected ? (std::to_string(status.tools.size()) + " tools").c_str() : status.error.c_str());

    std::unique_ptr<LlmProvider> provider;
    try
    {
        const std::string key = environment(profile->apiKeyEnv);
        if (profile->api == LlmApi::Anthropic)
        {
            AnthropicProvider::Settings settings;
            settings.baseUrl = profile->baseUrl;
            settings.model = profile->model;
            settings.apiKey = key;
            settings.vision = profile->vision;
            settings.temperature = profile->temperature;
            settings.stream = profile->stream;
            settings.timeoutSeconds = profile->requestTimeout;
            provider = std::make_unique<AnthropicProvider>(settings);
        }
        else
        {
            OpenAiProvider::Settings settings;
            settings.baseUrl = profile->baseUrl;
            settings.model = profile->model;
            settings.apiKey = key;
            settings.vision = profile->vision;
            settings.temperature = profile->temperature;
            settings.stream = profile->stream;
            settings.timeoutSeconds = profile->requestTimeout;
            provider = std::make_unique<OpenAiProvider>(settings);
        }
    }
    catch (const LlmError& failure)
    {
        std::fprintf(stderr, "%s\n", failure.what());
        return 2;
    }

    AgentConfig agentConfig;
    agentConfig.confirm = config.confirm;
    agentConfig.maxSteps = profile->maxSteps;
    agentConfig.simplifySchema = profile->simplifySchema;
    agentConfig.systemPrompt = profile->systemPrompt;
    agentConfig.prune.maxChars = profile->contextChars;

    Printer printer;
    // Nobody is here to ask, so a tool the policy covers is let through and said.
    Agent agent(*provider, hub, printer, agentConfig, [](const std::string& name, const Json&)
                {
                    std::printf("[allowed %s without asking]\n", name.c_str());
                    return true;
                });
    const RunResult run = agent.run(userContent(message, images), nullptr);
    static const char* const reasons[] = {"done", "cancelled", "max steps", "error"};
    std::printf("\n[%s after %d steps, %.0f s, %d failures]\n", reasons[static_cast<int>(run.reason)], run.steps,
                seconds(), printer.failures);
    return run.reason == RunReason::Done && printer.failures == 0 ? 0 : 1;
}
