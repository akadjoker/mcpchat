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
    openai.baseUrl = "https://api.openai.com/v1";
    openai.model = "gpt-4.1";
    openai.apiKeyEnv = "OPENAI_API_KEY";
    openai.vision = true;
    config.profiles = {ollama, lmstudio, openai};
    return config;
}

std::filesystem::path defaultConfigPath()
{
    return configDirectory() / "config.json";
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
                            {"base_url", item.baseUrl},
                            {"model", item.model},
                            {"api_key_env", item.apiKeyEnv},
                            {"vision", item.vision},
                            {"simplify_schema", item.simplifySchema},
                            {"stream", item.stream},
                            {"temperature", item.temperature ? Json(*item.temperature) : Json()},
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
        read.text("base_url", item.baseUrl);
        read.text("model", item.model);
        read.text("api_key_env", item.apiKeyEnv);
        read.flag("vision", item.vision);
        read.flag("simplify_schema", item.simplifySchema);
        read.flag("stream", item.stream);
        read.optionalNumber("temperature", item.temperature);
        read.number("max_steps", item.maxSteps);
        read.number("request_timeout", item.requestTimeout);
        read.number("context_chars", item.contextChars);
        read.text("system_prompt", item.systemPrompt);
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
