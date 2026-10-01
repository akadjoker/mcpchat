#ifndef MCPCHAT_CONFIG_H
#define MCPCHAT_CONFIG_H

#include "util/Json.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mcpchat
{

// An MCP server: `url` for Streamable HTTP, or `command` (+ `args`, `env`) for a stdio child process.
struct ServerConfig
{
    std::string name;
    std::string url;
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::map<std::string, std::string> headers;
    // Environment variable with a bearer token for an HTTP server; the token itself is never stored.
    std::string tokenEnv;
    bool enabled = true;
    double timeout = 300.0;

    std::vector<std::string> problems() const;
};

// An LLM endpoint that speaks the OpenAI chat-completions protocol (Ollama, LM Studio, vLLM, OpenAI, DeepSeek...).
struct Profile
{
    std::string name;
    std::string baseUrl;
    std::string model;
    // Environment variable with the key, never the key itself.
    std::string apiKeyEnv;
    // The model accepts images; stated, not guessed.
    bool vision = false;
    // For servers that reject oneOf/minItems/... in tool schemas.
    bool simplifySchema = false;
    bool stream = true;
    std::optional<double> temperature;
    int maxSteps = 40;
    double requestTimeout = 300.0;
    std::size_t contextChars = 120000;
    std::string systemPrompt;

    std::vector<std::string> problems() const;
};

enum class ConfirmPolicy
{
    Destructive,
    Writes,
    Never
};

const char* toString(ConfirmPolicy policy);
bool parseConfirmPolicy(const std::string& text, ConfirmPolicy& out);

struct Config
{
    std::vector<ServerConfig> servers;
    std::vector<Profile> profiles;
    std::string currentProfile;
    ConfirmPolicy confirm = ConfirmPolicy::Destructive;

    // The current profile, the first one when the name matches nothing, null without profiles.
    const Profile* profile() const;
    std::vector<std::string> problems() const;

    // What a first run writes: CocoShape over HTTP and three common LLM endpoints.
    static Config example();
};

std::filesystem::path defaultConfigPath();

// A missing file is an empty Config. False with `error` on unreadable or invalid JSON, unknown fields or wrong types.
bool loadConfig(const std::filesystem::path& path, Config& out, std::string& error);
bool saveConfig(const std::filesystem::path& path, const Config& config, std::string& error);

Json toJson(const Config& config);
bool fromJson(const Json& data, Config& out, std::string& error);

} // namespace mcpchat

#endif
