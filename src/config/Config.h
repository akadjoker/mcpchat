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

// An LLM endpoint that speaks the OpenAI chat-completions protocol (Ollama, LM Studio, vLLM, DeepSeek...), OpenAI's
// own Responses API (the newer reasoning models need it for tools), or the Anthropic Messages API (Claude).
enum class LlmApi
{
    OpenAi,
    OpenAiResponses,
    Anthropic
};

const char* toString(LlmApi api);
bool parseLlmApi(const std::string& text, LlmApi& out);

struct Profile
{
    std::string name;
    std::string baseUrl;
    std::string model;
    // Which protocol the endpoint speaks; the two are not interchangeable.
    LlmApi api = LlmApi::OpenAi;
    // Environment variable with the key, never the key itself.
    std::string apiKeyEnv;
    // The model accepts images; stated, not guessed.
    bool vision = false;
    // For servers that reject oneOf/minItems/... in tool schemas.
    bool simplifySchema = false;
    bool stream = true;
    std::optional<double> temperature;
    // How hard a reasoning model thinks ("low", "medium", "high"...), for the Responses API; empty is the model's own.
    std::string reasoningEffort;
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

// A provider worth offering in Settings, with the models it serves: the model names change with the season, so they
// are data in a file of their own rather than something the program has to know.
struct ProviderPreset
{
    std::string name;
    LlmApi api = LlmApi::OpenAi;
    std::string baseUrl;
    std::string apiKeyEnv;
    std::vector<std::string> models;
};

struct ProviderCatalog
{
    std::vector<ProviderPreset> providers;

    // The preset a profile looks like it came from, matched on the base URL; null when it was written by hand.
    const ProviderPreset* match(const Profile& profile) const;

    static ProviderCatalog example();
};

// Points a profile at a known provider: its protocol, URL and key variable. A model it does not serve would only
// fail later, so it moves to the first of the preset's; a profile with nothing to copy keeps what it has.
void applyPreset(const ProviderPreset& preset, Profile& profile);

std::filesystem::path defaultCatalogPath();

// Always fills `out`: the file when it parses, the built-in example otherwise, with `error` saying what went wrong. A
// missing file gets the example written next to the configuration.
void loadCatalog(const std::filesystem::path& path, ProviderCatalog& out, std::string& error);
bool saveCatalog(const std::filesystem::path& path, const ProviderCatalog& catalog, std::string& error);

} // namespace mcpchat

#endif
