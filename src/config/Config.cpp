#include "config/Config.h"

#include "util/Paths.h"
#include "util/Strings.h"

#include <algorithm>
#include <set>

namespace mcpchat
{

namespace
{
constexpr int kFileVersion = 1;

bool isHttpUrl(const std::string& text)
{
    return startsWith(text, "http://") || startsWith(text, "https://");
}

// Reads the known fields of one object, refusing unknown ones and wrong types.
class Reader
{
public:
    Reader(const Json& object, std::string where, std::string& error) : mObject(object), mWhere(std::move(where)), mError(error)
    {
        if (!object.is_object())
            fail("expected an object");
    }

    bool ok() const
    {
        return mOk;
    }

    void text(const char* key, std::string& out)
    {
        if (const Json* value = get(key))
        {
            if (value->is_string())
                out = value->get<std::string>();
            else
                fail(std::string("'") + key + "' must be a string");
        }
    }

    void flag(const char* key, bool& out)
    {
        if (const Json* value = get(key))
        {
            if (value->is_boolean())
                out = value->get<bool>();
            else
                fail(std::string("'") + key + "' must be true or false");
        }
    }

    template <typename Number> void number(const char* key, Number& out)
    {
        if (const Json* value = get(key))
        {
            if (value->is_number())
                out = value->get<Number>();
            else
                fail(std::string("'") + key + "' must be a number");
        }
    }

    void optionalNumber(const char* key, std::optional<double>& out)
    {
        if (const Json* value = get(key))
        {
            if (value->is_null())
                out.reset();
            else if (value->is_number())
                out = value->get<double>();
            else
                fail(std::string("'") + key + "' must be a number or null");
        }
    }

    void texts(const char* key, std::vector<std::string>& out)
    {
        if (const Json* value = get(key))
        {
            if (!value->is_array())
                return fail(std::string("'") + key + "' must be an array of strings");
            out.clear();
            for (const Json& item : *value)
            {
                if (!item.is_string())
                    return fail(std::string("'") + key + "' must be an array of strings");
                out.push_back(item.get<std::string>());
            }
        }
    }

    void table(const char* key, std::map<std::string, std::string>& out)
    {
        if (const Json* value = get(key))
        {
            if (!value->is_object())
                return fail(std::string("'") + key + "' must be an object of strings");
            out.clear();
            for (auto it = value->begin(); it != value->end(); ++it)
            {
                if (!it.value().is_string())
                    return fail(std::string("'") + key + "' must be an object of strings");
                out[it.key()] = it.value().get<std::string>();
            }
        }
    }

    // The raw value of a field read by hand; null when absent.
    const Json* raw(const char* key)
    {
        return get(key);
    }

    void finish()
    {
        if (!mOk || !mObject.is_object())
            return;
        std::vector<std::string> unknown;
        for (auto it = mObject.begin(); it != mObject.end(); ++it)
        {
            if (!mSeen.count(it.key()))
                unknown.push_back(it.key());
        }
        if (unknown.empty())
            return;
        std::string list;
        for (const std::string& name : unknown)
            list += (list.empty() ? "" : ", ") + name;
        fail("unknown field(s) " + list);
    }

private:
    const Json* get(const char* key)
    {
        mSeen.insert(key);
        if (!mOk || !mObject.is_object() || !mObject.contains(key))
            return nullptr;
        return &mObject[key];
    }

    void fail(const std::string& message)
    {
        if (mOk)
            mError = mWhere + ": " + message;
        mOk = false;
    }

    const Json& mObject;
    std::string mWhere;
    std::string& mError;
    std::set<std::string> mSeen;
    bool mOk = true;
};
} // namespace

std::vector<std::string> ServerConfig::problems() const
{
    std::vector<std::string> found;
    if (trim(name).empty())
        found.push_back("a server has no name");
    if (url.empty() == command.empty())
        found.push_back("server '" + name + "': give either 'url' or 'command'");
    if (!url.empty() && !isHttpUrl(url))
        found.push_back("server '" + name + "': 'url' must start with http:// or https://");
    return found;
}

std::vector<std::string> Profile::problems() const
{
    std::vector<std::string> found;
    if (trim(name).empty())
        found.push_back("a profile has no name");
    if (!isHttpUrl(baseUrl))
        found.push_back("profile '" + name + "': 'base_url' must start with http:// or https://");
    if (trim(model).empty())
        found.push_back("profile '" + name + "': 'model' is empty");
    if (maxSteps < 1)
        found.push_back("profile '" + name + "': 'max_steps' must be at least 1");
    return found;
}

const char* toString(ConfirmPolicy policy)
{
    switch (policy)
    {
    case ConfirmPolicy::Destructive:
        return "destructive";
    case ConfirmPolicy::Writes:
        return "writes";
    case ConfirmPolicy::Never:
        return "never";
    }
    return "destructive";
}

bool parseConfirmPolicy(const std::string& text, ConfirmPolicy& out)
{
    for (const ConfirmPolicy policy : {ConfirmPolicy::Destructive, ConfirmPolicy::Writes, ConfirmPolicy::Never})
    {
        if (text == toString(policy))
        {
            out = policy;
            return true;
        }
    }
    return false;
}

const char* toString(LlmApi api)
{
    switch (api)
    {
    case LlmApi::OpenAi:
        return "openai";
    case LlmApi::OpenAiResponses:
        return "openai-responses";
    case LlmApi::Anthropic:
        return "anthropic";
    }
    return "openai";
}

bool parseLlmApi(const std::string& text, LlmApi& out)
{
    for (const LlmApi api : {LlmApi::OpenAi, LlmApi::OpenAiResponses, LlmApi::Anthropic})
    {
        if (text == toString(api))
        {
            out = api;
            return true;
        }
    }
    return false;
}

const Profile* Config::profile() const
{
    for (const Profile& candidate : profiles)
    {
        if (candidate.name == currentProfile)
            return &candidate;
    }
    return profiles.empty() ? nullptr : &profiles.front();
}

std::vector<std::string> Config::problems() const
{
    std::vector<std::string> found;
    for (const ServerConfig& server : servers)
    {
        const std::vector<std::string> more = server.problems();
        found.insert(found.end(), more.begin(), more.end());
    }
    for (const Profile& item : profiles)
    {
        const std::vector<std::string> more = item.problems();
        found.insert(found.end(), more.begin(), more.end());
    }
    auto duplicates = [&found](const char* kind, std::vector<std::string> names)
    {
        std::sort(names.begin(), names.end());
        for (std::size_t i = 1; i < names.size(); ++i)
        {
            if (names[i] == names[i - 1] && (i + 1 == names.size() || names[i + 1] != names[i]))
                found.push_back(std::string("two ") + kind + "s are called '" + names[i] + "'");
        }
    };
    std::vector<std::string> serverNames, profileNames;
    for (const ServerConfig& server : servers)
        serverNames.push_back(server.name);
    for (const Profile& item : profiles)
        profileNames.push_back(item.name);
    duplicates("server", serverNames);
    duplicates("profile", profileNames);
    return found;
}

Config Config::example()
{
    Config config;
    config.currentProfile = "ollama";
    ServerConfig cocoshape;
    cocoshape.name = "cocoshape";
    cocoshape.url = "http://127.0.0.1:7420/mcp";
    cocoshape.tokenEnv = "COCOSHAPE_API_TOKEN";
    config.servers.push_back(cocoshape);
    Profile ollama;
    ollama.name = "ollama";
    ollama.baseUrl = "http://localhost:11434/v1";
    ollama.model = "qwen2.5:14b";
    Profile lmstudio;
    lmstudio.name = "lmstudio";
    lmstudio.baseUrl = "http://localhost:1234/v1";
    lmstudio.model = "local-model";
    Profile openai;
    openai.name = "openai";
    openai.api = LlmApi::OpenAiResponses;
    openai.baseUrl = "https://api.openai.com/v1";
    openai.model = "gpt-4.1";
    openai.apiKeyEnv = "OPENAI_API_KEY";
    openai.vision = true;
    Profile deepseek;
    deepseek.name = "deepseek";
    deepseek.baseUrl = "https://api.deepseek.com/v1";
    // The model it starts on, deepseek-flash, accepts images; the other one, deepseek-v4-pro, does not.
    deepseek.model = "deepseek-flash";
    deepseek.apiKeyEnv = "DEEPSEEK_API_KEY";
    deepseek.vision = true;
    Profile claude;
    claude.name = "claude";
    claude.api = LlmApi::Anthropic;
    claude.baseUrl = "https://api.anthropic.com/v1";
    claude.model = "claude-sonnet-5-5";
    claude.apiKeyEnv = "ANTHROPIC_API_KEY";
    claude.vision = true;
    config.profiles = {ollama, lmstudio, openai, deepseek, claude};
    return config;
}

std::filesystem::path defaultConfigPath()
{
    return configDirectory() / "config.json";
}

namespace
{
// Case and trailing slashes do not make two base URLs different.
std::string normalizeUrl(const std::string& url)
{
    std::string out = toLower(trim(url));
    while (!out.empty() && out.back() == '/')
        out.pop_back();
    return out;
}

Json toJson(const ProviderCatalog& catalog)
{
    Json providers = Json::array();
    for (const ProviderPreset& preset : catalog.providers)
    {
        providers.push_back({{"name", preset.name},
                             {"api", toString(preset.api)},
                             {"base_url", preset.baseUrl},
                             {"api_key_env", preset.apiKeyEnv},
                             {"models", preset.models}});
    }
    return {{"version", kFileVersion}, {"providers", providers}};
}

bool fromJson(const Json& data, ProviderCatalog& out, std::string& error)
{
    out = ProviderCatalog();
    Reader top(data, "the catalog", error);
    int version = kFileVersion;
    top.number("version", version);
    if (!top.ok())
        return false;
    const Json empty = Json::array();
    const Json* providersField = top.raw("providers");
    const Json& providers = providersField ? *providersField : empty;
    top.finish();
    if (!top.ok() || !providers.is_array())
    {
        if (top.ok())
            error = "'providers' must be an array";
        return false;
    }
    for (std::size_t i = 0; i < providers.size(); ++i)
    {
        ProviderPreset preset;
        Reader read(providers[i], "providers[" + std::to_string(i) + "]", error);
        read.text("name", preset.name);
        std::string api;
        read.text("api", api);
        read.text("base_url", preset.baseUrl);
        read.text("api_key_env", preset.apiKeyEnv);
        read.texts("models", preset.models);
        if (!api.empty() && !parseLlmApi(api, preset.api))
        {
            error = "providers[" + std::to_string(i) + "]: 'api' must be \"openai\", \"openai-responses\" or \"anthropic\"";
            return false;
        }
        read.finish();
        if (!read.ok())
            return false;
        if (trim(preset.name).empty())
        {
            error = "providers[" + std::to_string(i) + "]: 'name' is empty";
            return false;
        }
        out.providers.push_back(std::move(preset));
    }
    return true;
}
} // namespace

const ProviderPreset* ProviderCatalog::match(const Profile& profile) const
{
    const std::string wanted = normalizeUrl(profile.baseUrl);
    if (wanted.empty())
        return nullptr;
    for (const ProviderPreset& preset : providers)
        if (normalizeUrl(preset.baseUrl) == wanted && preset.api == profile.api)
            return &preset;
    return nullptr;
}

void applyPreset(const ProviderPreset& preset, Profile& profile)
{
    profile.api = preset.api;
    profile.baseUrl = preset.baseUrl;
    if (!preset.apiKeyEnv.empty())
        profile.apiKeyEnv = preset.apiKeyEnv;
    if (!preset.models.empty() &&
        std::find(preset.models.begin(), preset.models.end(), profile.model) == preset.models.end())
        profile.model = preset.models.front();
}

ProviderCatalog ProviderCatalog::example()
{
    ProviderCatalog catalog;
    ProviderPreset claude;
    claude.name = "Anthropic (Claude)";
    claude.api = LlmApi::Anthropic;
    claude.baseUrl = "https://api.anthropic.com/v1";
    claude.apiKeyEnv = "ANTHROPIC_API_KEY";
    claude.models = {"claude-opus-5-5", "claude-sonnet-5-5", "claude-fable-5-1", "claude-haiku-4-5-20251001"};
    ProviderPreset deepseek;
    deepseek.name = "DeepSeek";
    deepseek.baseUrl = "https://api.deepseek.com/v1";
    deepseek.apiKeyEnv = "DEEPSEEK_API_KEY";
    deepseek.models = {"deepseek-flash", "deepseek-v4-pro"};
    ProviderPreset openai;
    openai.name = "OpenAI";
    openai.api = LlmApi::OpenAiResponses;
    openai.baseUrl = "https://api.openai.com/v1";
    openai.apiKeyEnv = "OPENAI_API_KEY";
    // The Astra and Sol models only take function tools together with reasoning on /responses.
    openai.models = {"gpt-6-astra", "gpt-6-sol"};
    ProviderPreset ollama;
    ollama.name = "Ollama (local)";
    ollama.baseUrl = "http://localhost:11434/v1";
    ProviderPreset lmstudio;
    lmstudio.name = "LM Studio (local)";
    lmstudio.baseUrl = "http://localhost:1234/v1";
    catalog.providers = {claude, deepseek, openai, ollama, lmstudio};
    return catalog;
}

std::filesystem::path defaultCatalogPath()
{
    return configDirectory() / "providers.json";
}

void loadCatalog(const std::filesystem::path& path, ProviderCatalog& out, std::string& error)
{
    out = ProviderCatalog::example();
    error.clear();
    std::error_code code;
    if (!std::filesystem::exists(path, code))
    {
        // Written next to the configuration so the models can be corrected without touching the program.
        saveCatalog(path, out, error);
        return;
    }
    std::string text;
    if (!readTextFile(path, text))
    {
        error = path.u8string() + ": cannot read the file";
        return;
    }
    const Json data = Json::parse(text, nullptr, false);
    if (data.is_discarded())
    {
        error = path.u8string() + ": not valid JSON";
        return;
    }
    ProviderCatalog read;
    if (!fromJson(data, read, error))
    {
        error = path.u8string() + ": " + error;
        return;
    }
    out = std::move(read);
}

bool saveCatalog(const std::filesystem::path& path, const ProviderCatalog& catalog, std::string& error)
{
    return writeTextFileAtomic(path, toJson(catalog).dump(2) + "\n", &error);
}

Json toJson(const Config& config)
{
    Json servers = Json::array();
    for (const ServerConfig& server : config.servers)
    {
        servers.push_back({{"name", server.name},
                           {"url", server.url},
                           {"command", server.command},
                           {"args", server.args},
                           {"env", server.env},
                           {"headers", server.headers},
                           {"token_env", server.tokenEnv},
                           {"enabled", server.enabled},
                           {"timeout", server.timeout}});
    }
    Json profiles = Json::array();
    for (const Profile& item : config.profiles)
    {
        profiles.push_back({{"name", item.name},
                            {"api", toString(item.api)},
                            {"base_url", item.baseUrl},
                            {"model", item.model},
                            {"api_key_env", item.apiKeyEnv},
                            {"vision", item.vision},
                            {"simplify_schema", item.simplifySchema},
                            {"stream", item.stream},
                            {"temperature", item.temperature ? Json(*item.temperature) : Json()},
                            {"reasoning_effort", item.reasoningEffort},
                            {"max_steps", item.maxSteps},
                            {"request_timeout", item.requestTimeout},
                            {"context_chars", item.contextChars},
                            {"system_prompt", item.systemPrompt}});
    }
    return {{"version", kFileVersion},
            {"current_profile", config.currentProfile},
            {"confirm", toString(config.confirm)},
            {"servers", servers},
            {"profiles", profiles}};
}

bool fromJson(const Json& data, Config& out, std::string& error)
{
    out = Config();
    Reader top(data, "the configuration", error);
    int version = kFileVersion;
    top.number("version", version);
    top.text("current_profile", out.currentProfile);
    std::string confirm = "destructive";
    top.text("confirm", confirm);
    if (!top.ok())
        return false;
    if (!parseConfirmPolicy(confirm, out.confirm))
    {
        error = "'confirm' must be one of destructive, writes, never";
        return false;
    }
    const Json empty = Json::array();
    const Json* serversField = top.raw("servers");
    const Json* profilesField = top.raw("profiles");
    const Json& servers = serversField ? *serversField : empty;
    const Json& profiles = profilesField ? *profilesField : empty;
    top.finish();
    if (!top.ok() || !servers.is_array() || !profiles.is_array())
    {
        if (top.ok())
            error = "'servers' and 'profiles' must be arrays";
        return false;
    }
    for (std::size_t i = 0; i < servers.size(); ++i)
    {
        ServerConfig server;
        Reader read(servers[i], "servers[" + std::to_string(i) + "]", error);
        read.text("name", server.name);
        read.text("url", server.url);
        read.text("command", server.command);
        read.texts("args", server.args);
        read.table("env", server.env);
        read.table("headers", server.headers);
        read.text("token_env", server.tokenEnv);
        read.flag("enabled", server.enabled);
        read.number("timeout", server.timeout);
        read.finish();
        if (!read.ok())
            return false;
        out.servers.push_back(server);
    }
    for (std::size_t i = 0; i < profiles.size(); ++i)
    {
        Profile item;
        Reader read(profiles[i], "profiles[" + std::to_string(i) + "]", error);
        read.text("name", item.name);
        std::string api;
        read.text("api", api);
        read.text("base_url", item.baseUrl);
        read.text("model", item.model);
        read.text("api_key_env", item.apiKeyEnv);
        read.flag("vision", item.vision);
        read.flag("simplify_schema", item.simplifySchema);
        read.flag("stream", item.stream);
        read.optionalNumber("temperature", item.temperature);
        read.text("reasoning_effort", item.reasoningEffort);
        read.number("max_steps", item.maxSteps);
        read.number("request_timeout", item.requestTimeout);
        read.number("context_chars", item.contextChars);
        read.text("system_prompt", item.systemPrompt);
        if (!api.empty() && !parseLlmApi(api, item.api))
        {
            error = "profiles[" + std::to_string(i) + "]: 'api' must be \"openai\", \"openai-responses\" or \"anthropic\"";
            return false;
        }
        read.finish();
        if (!read.ok())
            return false;
        out.profiles.push_back(item);
    }
    return true;
}

bool loadConfig(const std::filesystem::path& path, Config& out, std::string& error)
{
    std::error_code code;
    if (!std::filesystem::exists(path, code))
    {
        out = Config();
        return true;
    }
    std::string text;
    if (!readTextFile(path, text))
    {
        error = path.u8string() + ": cannot read the file";
        return false;
    }
    const Json data = Json::parse(text, nullptr, false);
    if (data.is_discarded())
    {
        error = path.u8string() + ": not valid JSON";
        return false;
    }
    if (!fromJson(data, out, error))
    {
        error = path.u8string() + ": " + error;
        return false;
    }
    return true;
}

bool saveConfig(const std::filesystem::path& path, const Config& config, std::string& error)
{
    return writeTextFileAtomic(path, toJson(config).dump(2) + "\n", &error);
}

} // namespace mcpchat
