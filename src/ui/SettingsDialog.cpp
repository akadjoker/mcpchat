#include "ui/SettingsDialog.h"

#include "ui/ChatSession.h"
#include "util/Strings.h"

#include <algorithm>
#include <cmath>

namespace mcpchat
{

namespace
{
const ig::Color kDim(140, 144, 152);
const ig::Color kWarning(236, 180, 90);
const ig::Color kError(240, 110, 110);

ig::StringView view(const std::string& text)
{
    return ig::StringView(text.data(), text.size());
}

bool editString(ig::Context& ui, const char* id, std::string& value, const ig::Rect& bounds)
{
    ig::String buffer(value.data(), value.size());
    if (!ui.inputText(id, buffer, bounds))
        return false;
    value.assign(buffer.data(), buffer.size());
    return true;
}

// iGUI's bounded checkbox and radio take the box itself and draw the label after it; the label clicks too.
bool tick(ig::Context& ui, const char* label, bool checked, float x, float y, float row)
{
    const float box = std::floor(row * 0.6f);
    const ig::Rect square(x, y + (row - box) * 0.5f, box, box);
    const bool clicked = ui.radioButton(label, checked, square);
    const float labelWidth = ui.textWidth(label) + ui.theme().windowPadding;
    ui.pushId(label);
    const bool labelClicked = ui.invisibleButton("label", ig::Rect(x + box, y, labelWidth, row));
    ui.popId();
    return clicked || labelClicked;
}

bool checkbox(ig::Context& ui, const char* label, bool& value, float x, float y, float row)
{
    const float box = std::floor(row * 0.6f);
    const ig::Rect square(x, y + (row - box) * 0.5f, box, box);
    bool changed = ui.checkbox(label, value, square);
    const float labelWidth = ui.textWidth(label) + ui.theme().windowPadding;
    ui.pushId(label);
    if (ui.invisibleButton("label", ig::Rect(x + box, y, labelWidth, row)))
    {
        value = !value;
        changed = true;
    }
    ui.popId();
    return changed;
}

std::vector<std::string> splitLines(const ig::String& text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    const std::string all(text.data(), text.size());
    while (start <= all.size())
    {
        std::size_t end = all.find('\n', start);
        if (end == std::string::npos)
            end = all.size();
        std::string line = all.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines)
{
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i)
        out += (i ? "\n" : "") + lines[i];
    return out;
}

std::string joinPairs(const std::map<std::string, std::string>& pairs)
{
    std::vector<std::string> lines;
    for (const auto& pair : pairs)
        lines.push_back(pair.first + "=" + pair.second);
    return joinLines(lines);
}

// NAME=VALUE per line, blank lines skipped. False with `error` naming the first bad line.
bool parsePairs(const ig::String& text, const std::string& what, std::map<std::string, std::string>& out,
                std::string& error)
{
    out.clear();
    const std::vector<std::string> lines = splitLines(text);
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (trim(lines[i]).empty())
            continue;
        const std::size_t equals = lines[i].find('=');
        const std::string name = equals == std::string::npos ? std::string() : trim(lines[i].substr(0, equals));
        if (name.empty())
        {
            error = what + ", line " + std::to_string(i + 1) + ": expected NAME=VALUE";
            return false;
        }
        out[name] = lines[i].substr(equals + 1);
    }
    return true;
}

std::string uniqueName(const std::string& base, const std::vector<std::string>& taken)
{
    for (int n = 1;; ++n)
    {
        const std::string name = base + " " + std::to_string(n);
        if (std::find(taken.begin(), taken.end(), name) == taken.end())
            return name;
    }
}

// Where a secret comes from, for the label next to its field.
std::string secretSource(const SecretStore& secrets, const std::string& account, const std::string& environment)
{
    if (secrets.inMemory(account))
        return "set for this session";
    if (!environment.empty() && !secrets.resolve(account, environment).empty())
        return "from $" + environment;
    return "not set";
}

// Rows of a label and a field, top to bottom.
struct Form
{
    ig::Context& ui;
    float x;
    float width;
    float labelWidth;
    float row;
    float gap;
    float lineHeight;
    float y = 0.0f;

    ig::Rect field(const std::string& label, float height = 0.0f)
    {
        const float h = height > 0.0f ? height : row;
        ui.drawText(view(label), ig::Vec2(x, y + (row - lineHeight) * 0.5f), ui.theme().labelText);
        const ig::Rect rect(x + labelWidth, y, width - labelWidth, h);
        y += h + gap;
        return rect;
    }
};
} // namespace

void SettingsDialog::open(const Config& config)
{
    mDraft = config;
    mServerText.clear();
    for (const ServerConfig& server : mDraft.servers)
    {
        ServerText text;
        text.args = ig::String(joinLines(server.args));
        text.env = ig::String(joinPairs(server.env));
        text.headers = ig::String(joinPairs(server.headers));
        mServerText.push_back(std::move(text));
    }
    mProfile = 0;
    for (std::size_t i = 0; i < mDraft.profiles.size(); ++i)
    {
        if (&mDraft.profiles[i] == mDraft.profile())
            mProfile = static_cast<int>(i);
    }
    mServer = 0;
    mPromptProfile = -1;
    mKey.clear();
    mMessage.clear();
    mOpen = true;
}

bool SettingsDialog::commit(std::string& error)
{
    for (std::size_t i = 0; i < mDraft.servers.size(); ++i)
    {
        ServerConfig& server = mDraft.servers[i];
        const ServerText& text = mServerText[i];
        server.args.clear();
        for (const std::string& line : splitLines(text.args))
        {
            if (!trim(line).empty())
                server.args.push_back(line);
        }
        const std::string where = "server '" + server.name + "'";
        if (!parsePairs(text.env, where + " environment", server.env, error) ||
            !parsePairs(text.headers, where + " headers", server.headers, error))
            return false;
    }
    return true;
}

void SettingsDialog::draw(ig::Context& ui, ChatSession& session, const ig::Vec2& display)
{
    const ig::Theme& theme = ui.theme();
    const ig::Vec2 size(std::min(780.0f, display.x - 24.0f), std::min(640.0f, display.y - 24.0f));
    // Begun even when closed: that is what releases iGUI's modal hold on the window.
    if (!ui.beginDialog("Settings", mOpen, size))
        return;
    mLeft = theme.windowPadding;
    const float top = theme.windowPadding * 0.5f;
    const float width = ui.availableWidth() - mLeft * 2.0f;
    const float height = ui.availableHeight() - top - mLeft;
    const float row = theme.widgetHeight;
    const float gap = theme.itemSpacing;

    static const ig::StringView tabs[] = {"Model", "Servers", "General"};
    if (ui.tabBar("tabs", mTab, ig::Span<const ig::StringView>(tabs, 3), ig::Rect(mLeft, top, width, row)))
        mKey.clear();
    const float bodyTop = top + row + gap * 2.0f;
    const float bodyHeight = top + height - bodyTop - row - gap * 2.0f;
    ui.pushId(static_cast<std::uint64_t>(mTab + 1));
    if (mTab == 0)
        drawProfiles(ui, session, bodyTop, width, bodyHeight);
    else if (mTab == 1)
        drawServers(ui, session, bodyTop, width, bodyHeight);
    else
        drawGeneral(ui, session, bodyTop, width);
    ui.popId();

    const float buttonsTop = top + height - row;
    const float buttonWidth = 100.0f;
    const std::vector<std::string> problems = mDraft.problems();
    const std::string& note = !mMessage.empty() ? mMessage : problems.empty() ? std::string() : problems.front();
    if (!note.empty())
    {
        const float lineHeight = ui.theme().fontSize * 1.25f;
        ui.drawText(view(note), ig::Vec2(mLeft, buttonsTop + (row - lineHeight) * 0.5f),
                    mMessage.empty() ? kWarning : kError);
    }
    if (ui.button("Save", ig::Rect(mLeft + width - buttonWidth, buttonsTop, buttonWidth, row)))
    {
        std::string error;
        if (!commit(error) || !session.apply(mDraft, error))
            mMessage = error;
        else
            mOpen = false;
    }
    if (ui.button("Cancel", ig::Rect(mLeft + width - buttonWidth * 2.0f - gap, buttonsTop, buttonWidth, row)))
        mOpen = false;
    ui.endDialog();
}

void SettingsDialog::drawProfiles(ig::Context& ui, ChatSession& session, float top, float width, float height)
{
    const ig::Theme& theme = ui.theme();
    const float row = theme.widgetHeight;
    const float gap = theme.itemSpacing;
    const float listWidth = 190.0f;

    std::vector<std::string> names;
    for (const Profile& profile : mDraft.profiles)
        names.push_back(profile.name.empty() ? "(no name)" : profile.name);
    std::vector<ig::StringView> items;
    for (const std::string& name : names)
        items.push_back(view(name));
    if (ui.listBox("list", mProfile, ig::Span<const ig::StringView>(items.data(), items.size()),
               ig::Rect(mLeft, top, listWidth, height - row - gap)))
        mKey.clear();
    const float half = (listWidth - gap) * 0.5f;
    if (ui.button("Add", ig::Rect(mLeft, top + height - row, half, row)))
    {
        std::vector<std::string> taken;
        for (const Profile& profile : mDraft.profiles)
            taken.push_back(profile.name);
        Profile profile;
        profile.name = uniqueName("profile", taken);
        mDraft.profiles.push_back(profile);
        mProfile = static_cast<int>(mDraft.profiles.size()) - 1;
    }
    if (ui.button("Remove", ig::Rect(mLeft + half + gap, top + height - row, half, row)) && !mDraft.profiles.empty())
    {
        mDraft.profiles.erase(mDraft.profiles.begin() + std::clamp(mProfile, 0, static_cast<int>(mDraft.profiles.size()) - 1));
        mPromptProfile = -1;
    }
    if (mDraft.profiles.empty())
        return;
    mProfile = std::clamp(mProfile, 0, static_cast<int>(mDraft.profiles.size()) - 1);
    Profile& profile = mDraft.profiles[static_cast<std::size_t>(mProfile)];
    if (mPromptProfile != mProfile)
    {
        mSystemPrompt = ig::String(profile.systemPrompt);
        mPromptProfile = mProfile;
        mKey.clear();
    }

    const float lineHeight = theme.fontSize * 1.25f;
    Form form{ui, mLeft + listWidth + gap * 3.0f, width - listWidth - gap * 3.0f, 130.0f, row, gap, lineHeight, top};
    const std::string oldName = profile.name;
    if (editString(ui, "name", profile.name, form.field("Name")) && mDraft.currentProfile == oldName)
        mDraft.currentProfile = profile.name;
    editString(ui, "base_url", profile.baseUrl, form.field("Base URL"));
    editString(ui, "model", profile.model, form.field("Model"));
    editString(ui, "key_env", profile.apiKeyEnv, form.field("API key variable"));

    const ig::Rect keyRect = form.field("API key");
    const float setWidth = 60.0f;
    ui.inputText("key", mKey, ig::Rect(keyRect.x, keyRect.y, keyRect.width - setWidth - gap, row));
    if (ui.button("Set", ig::Rect(keyRect.x + keyRect.width - setWidth, keyRect.y, setWidth, row)) && !mKey.empty())
    {
        session.secrets().setMemory(SecretStore::llmAccount(profile.name), std::string(mKey.data(), mKey.size()));
        mKey.clear();
    }
    const std::string source =
        "Key: " + secretSource(session.secrets(), SecretStore::llmAccount(profile.name), profile.apiKeyEnv) +
        ". A key typed here is kept in memory only.";
    ui.drawText(view(source), ig::Vec2(keyRect.x, form.y), kDim);
    form.y += lineHeight + gap;

    const ig::Rect options = form.field("Options");
    const float third = options.width / 3.0f;
    checkbox(ui, "Images", profile.vision, options.x, options.y, row);
    checkbox(ui, "Stream", profile.stream, options.x + third, options.y, row);
    checkbox(ui, "Simple schemas", profile.simplifySchema, options.x + third * 2.0f, options.y, row);

    const ig::Rect temperatureRect = form.field("Temperature");
    bool custom = profile.temperature.has_value();
    if (checkbox(ui, "Custom", custom, temperatureRect.x, temperatureRect.y, row))
        profile.temperature = custom ? std::optional<double>(0.7) : std::nullopt;
    if (profile.temperature)
    {
        float value = static_cast<float>(*profile.temperature);
        if (ui.sliderFloat("temperature", value, 0.0f, 2.0f,
                           ig::Rect(temperatureRect.x + third, temperatureRect.y, temperatureRect.width - third, row)))
            profile.temperature = std::round(value * 100.0f) / 100.0f;
    }

    const ig::Rect steps = form.field("Max steps");
    ui.pushId("steps");
    ui.stepperInt("", profile.maxSteps, 1, 500, ig::Rect(steps.x, steps.y, row * 3.7f, row));
    ui.popId();
    int timeout = static_cast<int>(profile.requestTimeout);
    if (ui.inputInt("timeout", timeout, form.field("Timeout (s)")))
        profile.requestTimeout = std::max(1, timeout);

    int context = static_cast<int>(std::min<std::size_t>(profile.contextChars, 2000000000u));
    if (ui.inputInt("context", context, form.field("Context (chars)")))
        profile.contextChars = static_cast<std::size_t>(std::max(1000, context));

    const float promptHeight = std::max(row * 2.0f, top + height - form.y);
    if (ui.inputTextMultiline("prompt", mSystemPrompt, form.field("System prompt", promptHeight)))
        profile.systemPrompt.assign(mSystemPrompt.data(), mSystemPrompt.size());
}

void SettingsDialog::drawServers(ig::Context& ui, ChatSession& session, float top, float width, float height)
{
    const ig::Theme& theme = ui.theme();
    const float row = theme.widgetHeight;
    const float gap = theme.itemSpacing;
    const float listWidth = 190.0f;

    std::vector<std::string> names;
    for (const ServerConfig& server : mDraft.servers)
        names.push_back(server.name.empty() ? "(no name)" : server.name);
    std::vector<ig::StringView> items;
    for (const std::string& name : names)
        items.push_back(view(name));
    if (ui.listBox("list", mServer, ig::Span<const ig::StringView>(items.data(), items.size()),
               ig::Rect(mLeft, top, listWidth, height - row - gap)))
        mKey.clear();
    const float half = (listWidth - gap) * 0.5f;
    if (ui.button("Add", ig::Rect(mLeft, top + height - row, half, row)))
    {
        std::vector<std::string> taken;
        for (const ServerConfig& server : mDraft.servers)
            taken.push_back(server.name);
        ServerConfig server;
        server.name = uniqueName("server", taken);
        mDraft.servers.push_back(server);
        mServerText.emplace_back();
        mServer = static_cast<int>(mDraft.servers.size()) - 1;
    }
    if (ui.button("Remove", ig::Rect(mLeft + half + gap, top + height - row, half, row)) && !mDraft.servers.empty())
    {
        const int doomed = std::clamp(mServer, 0, static_cast<int>(mDraft.servers.size()) - 1);
        mDraft.servers.erase(mDraft.servers.begin() + doomed);
        mServerText.erase(mServerText.begin() + doomed);
    }
    if (mDraft.servers.empty())
        return;
    mServer = std::clamp(mServer, 0, static_cast<int>(mDraft.servers.size()) - 1);
    ServerConfig& server = mDraft.servers[static_cast<std::size_t>(mServer)];
    ServerText& text = mServerText[static_cast<std::size_t>(mServer)];
    ui.pushId(static_cast<std::uint64_t>(mServer + 1));

    const float lineHeight = theme.fontSize * 1.25f;
    Form form{ui, mLeft + listWidth + gap * 3.0f, width - listWidth - gap * 3.0f, 130.0f, row, gap, lineHeight, top};
    const ig::Rect nameRect = form.field("Name");
    const float enabledWidth = 110.0f;
    editString(ui, "name", server.name, ig::Rect(nameRect.x, nameRect.y, nameRect.width - enabledWidth - gap, row));
    checkbox(ui, "Enabled", server.enabled, nameRect.x + nameRect.width - enabledWidth, nameRect.y, row);
    editString(ui, "url", server.url, form.field("URL (HTTP)"));
    editString(ui, "command", server.command, form.field("Command (stdio)"));
    const float multiHeight = std::max(row * 2.0f, lineHeight * 3.0f + theme.textEditPadding * 2.0f);
    ui.inputTextMultiline("args", text.args, form.field("Arguments", multiHeight));
    ui.inputTextMultiline("env", text.env, form.field("Environment", multiHeight));
    ui.inputTextMultiline("headers", text.headers, form.field("Headers", multiHeight));
    editString(ui, "token_env", server.tokenEnv, form.field("Token variable"));

    const ig::Rect tokenRect = form.field("Token");
    const float setWidth = 60.0f;
    ui.inputText("token", mKey, ig::Rect(tokenRect.x, tokenRect.y, tokenRect.width - setWidth - gap, row));
    if (ui.button("Set", ig::Rect(tokenRect.x + tokenRect.width - setWidth, tokenRect.y, setWidth, row)) && !mKey.empty())
    {
        session.secrets().setMemory(SecretStore::serverAccount(server.name), std::string(mKey.data(), mKey.size()));
        mKey.clear();
    }
    int timeout = static_cast<int>(server.timeout);
    if (ui.inputInt("timeout", timeout, form.field("Tool timeout (s)")))
        server.timeout = std::max(1, timeout);
    const std::string hint = "One argument per line; environment and headers as NAME=VALUE. Token: " +
                             secretSource(session.secrets(), SecretStore::serverAccount(server.name), server.tokenEnv) +
                             ".";
    ui.drawText(view(hint), ig::Vec2(form.x, form.y), kDim);
    ui.popId();
}

void SettingsDialog::drawGeneral(ig::Context& ui, ChatSession& session, float top, float width)
{
    const ig::Theme& theme = ui.theme();
    const float row = theme.widgetHeight;
    const float gap = theme.itemSpacing;
    const float lineHeight = theme.fontSize * 1.25f;
    Form form{ui, mLeft, width, 160.0f, row, gap, lineHeight, top};

    std::vector<std::string> names;
    int current = 0;
    for (std::size_t i = 0; i < mDraft.profiles.size(); ++i)
    {
        names.push_back(mDraft.profiles[i].name);
        if (&mDraft.profiles[i] == mDraft.profile())
            current = static_cast<int>(i);
    }
    std::vector<ig::StringView> items;
    for (const std::string& name : names)
        items.push_back(view(name));
    const ig::Rect profileRect = form.field("Profile in use");
    if (!items.empty() &&
        ui.comboBox("current", current, ig::Span<const ig::StringView>(items.data(), items.size()),
                    ig::Rect(profileRect.x, profileRect.y, std::min(profileRect.width, 300.0f), row)))
        mDraft.currentProfile = names[static_cast<std::size_t>(current)];

    form.y += gap;
    ui.drawText("Ask before running", ig::Vec2(mLeft, form.y), theme.labelText);
    form.y += lineHeight + gap;
    struct Choice
    {
        ConfirmPolicy policy;
        const char* label;
        const char* detail;
    };
    static const Choice choices[] = {
        {ConfirmPolicy::Destructive, "Destructive tools", "tools the server marks as destructive"},
        {ConfirmPolicy::Writes, "Every tool that changes something", "all but the tools marked read-only"},
        {ConfirmPolicy::Never, "Never", "run every tool without asking"},
    };
    for (const Choice& choice : choices)
    {
        if (tick(ui, choice.label, mDraft.confirm == choice.policy, mLeft + 16.0f, form.y, row))
            mDraft.confirm = choice.policy;
        ui.drawText(choice.detail, ig::Vec2(mLeft + 340.0f, form.y + (row - lineHeight) * 0.5f), kDim);
        form.y += row + gap;
    }

    form.y += gap;
    const std::string where = "Configuration file: " + session.configPath().u8string();
    ui.drawText(view(where), ig::Vec2(mLeft, form.y), kDim);
}

} // namespace mcpchat
