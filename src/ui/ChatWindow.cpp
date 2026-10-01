#include "ui/ChatWindow.h"

#include "render/GlBackend.h"
#include "ui/TextWrap.h"
#include "util/Paths.h"
#include "util/Strings.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace mcpchat
{

namespace
{
const ig::Color kUserBubble(42, 54, 76);
const ig::Color kAssistantBubble(38, 40, 46);
const ig::Color kToolBubble(33, 35, 40);
const ig::Color kErrorBubble(72, 36, 38);
const ig::Color kCodeBackground(24, 26, 30);
const ig::Color kUserName(120, 170, 255);
const ig::Color kAssistantName(120, 205, 145);
const ig::Color kToolName(232, 182, 92);
const ig::Color kErrorName(240, 110, 110);
const ig::Color kDim(140, 144, 152);
const ig::Color kHeading(150, 200, 255);
const ig::Color kConnected(96, 200, 120);
const ig::Color kDisconnected(225, 90, 90);

constexpr float kBubblePadding = 10.0f;
constexpr float kEntryGap = 8.0f;
constexpr float kCodeInset = 6.0f;
constexpr float kMaxImageHeight = 420.0f;

ig::StringView view(const std::string& text)
{
    return ig::StringView(text.data(), text.size());
}

std::string expandTabs(const std::string& text)
{
    if (text.find('\t') == std::string::npos)
        return text;
    std::string out;
    out.reserve(text.size() + 16);
    for (const char c : text)
    {
        if (c == '\t')
            out += "    ";
        else
            out += c;
    }
    return out;
}

std::string timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
    return text;
}

// Shown size of an image: at most the text width and kMaxImageHeight, never enlarged.
ig::Vec2 imageSize(const ChatImage& image, float width)
{
    if (image.width <= 0 || image.height <= 0)
        return ig::Vec2(0.0f, 0.0f);
    float shownWidth = std::min(width, static_cast<float>(image.width));
    float shownHeight = shownWidth * static_cast<float>(image.height) / static_cast<float>(image.width);
    if (shownHeight > kMaxImageHeight)
    {
        shownWidth *= kMaxImageHeight / shownHeight;
        shownHeight = kMaxImageHeight;
    }
    return ig::Vec2(shownWidth, shownHeight);
}
} // namespace

ChatWindow::ChatWindow(ChatSession& session, GlBackend& backend, ig::Context& ui)
    : mSession(session), mBackend(backend), mUi(ui)
{
}

void ChatWindow::wheel(float steps)
{
    mWheel += steps;
}

void ChatWindow::submit()
{
    mSubmit = true;
}

void ChatWindow::draw()
{
    const ig::Theme& theme = mUi.theme();
    const ig::FontAtlas& atlas = mBackend.fontAtlas();
    mLineHeight = std::ceil(atlas.lineHeight() * theme.fontSize / atlas.bakedSize());

    if (mUi.beginMainWindow("mcpchat"))
    {
        const float contentWidth = mUi.availableWidth();
        const float contentHeight = mUi.availableHeight();
        mDisplay = ig::Vec2(contentWidth + theme.windowPadding * 2.0f, contentHeight + theme.windowPadding * 2.0f);
        const float row = theme.widgetHeight;
        const float gap = theme.itemSpacing;
        const float inputHeight = mLineHeight * 4.0f + theme.textEditPadding * 2.0f;
        const float statusHeight = mLineHeight;
        const float transcriptTop = row + gap;
        const float transcriptHeight = contentHeight - transcriptTop - gap - inputHeight - gap - statusHeight;

        drawTopBar(contentWidth);
        if (transcriptHeight > mLineHeight)
            drawTranscript(transcriptTop, contentWidth, transcriptHeight);
        drawInput(contentHeight - statusHeight - gap - inputHeight, contentWidth, inputHeight);

        const float statusTop = contentHeight - statusHeight;
        mUi.drawText(view(mSession.status()), ig::Vec2(0.0f, statusTop), kDim);
        static const std::string hint = "Ctrl+Enter sends";
        mUi.drawText(view(hint), ig::Vec2(contentWidth - mUi.textWidth(view(hint)), statusTop), kDim);
        mUi.endWindow();
    }
    mWheel = 0.0f;

    mSettings.draw(mUi, mSession, mDisplay);
    drawConfirmation();
}

void ChatWindow::drawTopBar(float width)
{
    const ig::Theme& theme = mUi.theme();
    const float row = theme.widgetHeight;
    const float gap = theme.itemSpacing;
    const float textTop = (row - mLineHeight) * 0.5f;
    const bool busy = mSession.busy();
    auto buttonWidth = [&](const char* label) { return mUi.textWidth(label) + theme.padding * 4.0f; };

    float right = width;
    auto rightButton = [&](const char* label) -> bool
    {
        const float w = buttonWidth(label);
        right -= w;
        const bool clicked = mUi.button(label, ig::Rect(right, 0.0f, w, row));
        right -= gap;
        return clicked;
    };
    if (rightButton("Settings"))
        mSettings.open(mSession.config());
    if (rightButton("Save chat"))
        saveConversation();
    if (rightButton("New chat") && !busy)
    {
        mSession.newConversation();
        mLayouts.clear();
        mScroll = 0.0f;
        mStick = true;
    }
    if (rightButton("Reconnect") && !busy)
        mSession.connect();

    static const std::string modelLabel = "Model";
    mUi.drawText(view(modelLabel), ig::Vec2(0.0f, textTop), theme.labelText);
    float x = mUi.textWidth(view(modelLabel)) + gap;

    const Config& config = mSession.config();
    std::vector<std::string> names;
    names.reserve(config.profiles.size());
    int current = -1;
    const Profile* chosen = config.profile();
    for (std::size_t i = 0; i < config.profiles.size(); ++i)
    {
        const Profile& profile = config.profiles[i];
        names.push_back(profile.name + "  (" + profile.model + ")");
        if (&profile == chosen)
            current = static_cast<int>(i);
    }
    std::vector<ig::StringView> items;
    items.reserve(names.size());
    for (const std::string& name : names)
        items.push_back(view(name));
    const float comboWidth = std::min(300.0f, std::max(160.0f, right - x - gap));
    int selected = current;
    if (!items.empty() &&
        mUi.comboBox("profile", selected, ig::Span<const ig::StringView>(items.data(), items.size()),
                     ig::Rect(x, 0.0f, comboWidth, row)) &&
        selected != current && !busy)
        chooseProfile(selected);
    if (items.empty())
    {
        static const std::string none = "no profile: open Settings";
        mUi.drawText(view(none), ig::Vec2(x, textTop), kDim);
    }
    x += comboWidth + gap * 3.0f;

    const std::vector<ServerStatus>& servers = mSession.servers();
    if (servers.empty())
    {
        const std::string text = config.servers.empty() ? "no MCP servers" : "servers: not connected";
        if (x + mUi.textWidth(view(text)) < right)
            mUi.drawText(view(text), ig::Vec2(x, textTop), kDim);
        return;
    }
    for (std::size_t i = 0; i < servers.size(); ++i)
    {
        const ServerStatus& server = servers[i];
        const float itemWidth = 14.0f + mUi.textWidth(view(server.name));
        if (x + itemWidth > right)
            break;
        mUi.drawCircleFilled(ig::Vec2(x + 4.0f, row * 0.5f), 4.0f, server.connected ? kConnected : kDisconnected);
        mUi.drawText(view(server.name), ig::Vec2(x + 14.0f, textTop), theme.textColor);
        mUi.pushId(static_cast<std::uint64_t>(i));
        mUi.invisibleButton("server", ig::Rect(x, 0.0f, itemWidth, row));
        std::string tip;
        if (server.connected)
        {
            tip = (server.title.empty() ? server.name : server.title) + ": " + std::to_string(server.tools.size()) +
                  " tool(s)";
        }
        else
        {
            tip = server.name + ": " + server.error;
        }
        mUi.tooltip(view(tip));
        mUi.popId();
        x += itemWidth + gap * 2.0f;
    }
}

std::vector<ChatWindow::Line> ChatWindow::wrap(const std::string& text, float width, Format format) const
{
    std::vector<Line> lines;
    const std::string expanded = expandTabs(text);
    const auto measure = [this](std::string_view piece)
    { return mUi.textWidth(ig::StringView(piece.data(), piece.size())); };
    const bool markdown = format == Format::Markdown;
    bool inCode = format == Format::Code;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t end = expanded.find('\n', start);
        std::string_view source(expanded.data() + start,
                                (end == std::string::npos ? expanded.size() : end) - start);
        if (!source.empty() && source.back() == '\r')
            source.remove_suffix(1);
        const std::size_t first = source.find_first_not_of(' ');
        if (markdown && first != std::string_view::npos && source.substr(first, 3) == "```")
        {
            inCode = !inCode;
            lines.push_back({std::string(source), LineStyle::Fence});
        }
        else if (inCode)
        {
            for (std::string& piece : wrapText(source, std::max(width - kCodeInset * 2.0f, 1.0f), measure))
                lines.push_back({std::move(piece), LineStyle::Code});
        }
        else
        {
            LineStyle style = LineStyle::Text;
            std::string shown(source);
            if (markdown)
            {
                // One font, so emphasis markers go and headings change colour instead of size.
                const std::size_t hashes = shown.find_first_not_of('#');
                if (hashes > 0 && hashes <= 6 && hashes < shown.size() && shown[hashes] == ' ')
                {
                    shown.erase(0, hashes + 1);
                    style = LineStyle::Heading;
                }
                for (std::size_t at = shown.find("**"); at != std::string::npos; at = shown.find("**", at))
                    shown.erase(at, 2);
            }
            for (std::string& piece : wrapText(shown, std::max(width, 1.0f), measure))
                lines.push_back({std::move(piece), style});
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return lines;
}

void ChatWindow::relayout(Layout& layout, ChatEntry& entry, float width)
{
    if (layout.kind == entry.kind && layout.textSize == entry.text.size() &&
        layout.argumentsSize == entry.arguments.size() && layout.images == entry.images.size() &&
        layout.finished == entry.finished && layout.expanded == entry.expanded && layout.width == width)
        return;
    layout.kind = entry.kind;
    layout.textSize = entry.text.size();
    layout.argumentsSize = entry.arguments.size();
    layout.images = entry.images.size();
    layout.finished = entry.finished;
    layout.expanded = entry.expanded;
    layout.width = width;
    layout.body.clear();
    layout.arguments.clear();
    layout.imageHeights.clear();

    const float inner = width - kBubblePadding * 2.0f;
    switch (entry.kind)
    {
    case ChatEntry::Kind::Notice:
        layout.body = wrap(entry.text, width, Format::Plain);
        layout.height = static_cast<float>(layout.body.size()) * mLineHeight;
        break;
    case ChatEntry::Kind::Tool:
        layout.height = kBubblePadding * 2.0f + mLineHeight;
        if (entry.expanded)
        {
            layout.arguments = wrap(entry.arguments, inner, Format::Code);
            layout.body = wrap(entry.text, inner, Format::Code);
            layout.height += (static_cast<float>(layout.arguments.size() + layout.body.size()) + 2.0f) * mLineHeight +
                             kEntryGap * 2.0f;
            for (const auto& image : entry.images)
            {
                const float shown = imageSize(*image, inner).y;
                layout.imageHeights.push_back(shown);
                layout.height += shown + kEntryGap;
            }
        }
        break;
    default:
        layout.body = wrap(entry.text, inner, entry.kind == ChatEntry::Kind::Assistant ? Format::Markdown : Format::Plain);
        layout.height = kBubblePadding * 2.0f + mLineHeight + 4.0f +
                        static_cast<float>(layout.body.size()) * mLineHeight;
        break;
    }
}

float ChatWindow::drawLines(const std::vector<Line>& lines, float x, float top, float width, const ig::Color& color,
                            float viewTop, float viewBottom)
{
    const float height = static_cast<float>(lines.size()) * mLineHeight;
    if (lines.empty() || top > viewBottom || top + height < viewTop)
        return height;
    const std::size_t first = top >= viewTop ? 0 : static_cast<std::size_t>((viewTop - top) / mLineHeight);
    const std::size_t last = std::min(lines.size(), static_cast<std::size_t>((viewBottom - top) / mLineHeight) + 1);
    for (std::size_t i = first; i < last; ++i)
    {
        const Line& line = lines[i];
        const float y = top + static_cast<float>(i) * mLineHeight;
        if (line.style == LineStyle::Text || line.style == LineStyle::Heading)
        {
            mUi.drawText(view(line.text), ig::Vec2(x, y), line.style == LineStyle::Heading ? kHeading : color);
            continue;
        }
        mUi.drawRectFilled(ig::Rect(x, y, width, mLineHeight), kCodeBackground);
        mUi.drawText(view(line.text), ig::Vec2(x + kCodeInset, y), line.style == LineStyle::Fence ? kDim : color);
    }
    return height;
}

void ChatWindow::uploadImages(ChatEntry& entry)
{
    for (const auto& image : entry.images)
    {
        if (image->texture || image->rgba.empty())
            continue;
        image->texture = mBackend.createTexture(image->rgba.data(), image->width, image->height).value;
        image->rgba.clear();
        image->rgba.shrink_to_fit();
    }
}

float ChatWindow::drawEntry(std::size_t index, ChatEntry& entry, const Layout& layout, float top, float width,
                            float viewTop, float viewBottom)
{
    const ig::Theme& theme = mUi.theme();
    const ig::Rect box(0.0f, top, width, layout.height);
    const float inner = width - kBubblePadding * 2.0f;
    const float x = kBubblePadding;
    mUi.pushId(static_cast<std::uint64_t>(index));

    if (entry.kind == ChatEntry::Kind::Notice)
    {
        drawLines(layout.body, 0.0f, top, width, kDim, viewTop, viewBottom);
        mUi.popId();
        return layout.height;
    }

    if (entry.kind == ChatEntry::Kind::Tool)
    {
        mUi.drawRectFilledRounded(box, theme.borderRadius, kToolBubble);
        mUi.drawRectRounded(box, theme.borderRadius, theme.borderColor);
        const bool overCopy = drawCopyButton(entry, box);
        const float headerTop = top + kBubblePadding;
        const std::string marker = entry.expanded ? "[-]" : "[+]";
        mUi.drawText(view(marker), ig::Vec2(x, headerTop), kDim);
        const float nameX = x + mUi.textWidth(view(marker)) + 8.0f;
        mUi.drawText(view(entry.toolName), ig::Vec2(nameX, headerTop), kToolName);
        const std::string state = !entry.finished ? "running..." : entry.isError ? "failed" : "done";
        mUi.drawText(view(state), ig::Vec2(nameX + mUi.textWidth(view(entry.toolName)) + 12.0f, headerTop),
                     entry.isError ? kErrorName : kDim);
        if (!overCopy && mUi.invisibleButton("toggle", ig::Rect(0.0f, top, width, mLineHeight + kBubblePadding * 2.0f)))
            entry.expanded = !entry.expanded;

        if (entry.expanded)
        {
            float y = headerTop + mLineHeight + kEntryGap;
            static const std::string argumentsLabel = "Arguments";
            static const std::string resultLabel = "Result";
            mUi.drawText(view(argumentsLabel), ig::Vec2(x, y), kDim);
            y += mLineHeight;
            y += drawLines(layout.arguments, x, y, inner, theme.textColor, viewTop, viewBottom) + kEntryGap;
            mUi.drawText(view(resultLabel), ig::Vec2(x, y), kDim);
            y += mLineHeight;
            y += drawLines(layout.body, x, y, inner, entry.isError ? kErrorName : theme.textColor, viewTop, viewBottom);
            for (std::size_t i = 0; i < entry.images.size() && i < layout.imageHeights.size(); ++i)
            {
                y += kEntryGap;
                const ig::Vec2 size = imageSize(*entry.images[i], inner);
                if (y + size.y >= viewTop && y <= viewBottom)
                {
                    uploadImages(entry);
                    if (entry.images[i]->texture)
                        mUi.image(ig::TextureId(entry.images[i]->texture), ig::Rect(x, y, size.x, size.y));
                }
                y += layout.imageHeights[i];
            }
        }
    }
    else
    {
        const bool user = entry.kind == ChatEntry::Kind::User;
        const bool error = entry.kind == ChatEntry::Kind::Error;
        mUi.drawRectFilledRounded(box, theme.borderRadius, user ? kUserBubble : error ? kErrorBubble : kAssistantBubble);
        drawCopyButton(entry, box);
        const std::string role = user ? "You" : error ? "Error" : "Assistant";
        mUi.drawText(view(role), ig::Vec2(x, top + kBubblePadding),
                     user ? kUserName : error ? kErrorName : kAssistantName);
        drawLines(layout.body, x, top + kBubblePadding + mLineHeight + 4.0f, inner, theme.textColor, viewTop,
                  viewBottom);
    }

    mUi.popId();
    return layout.height;
}

bool ChatWindow::drawCopyButton(const ChatEntry& entry, const ig::Rect& box)
{
    if ((entry.text.empty() && entry.arguments.empty()) || mDragging || mSettings.isOpen() || mConfirmOpen ||
        !ig::contains(box, mUi.pointerContentPosition()))
        return false;
    const ig::Theme& theme = mUi.theme();
    static const std::string copyLabel = "Copy";
    const float w = mUi.textWidth(view(copyLabel)) + theme.padding * 2.0f;
    const ig::Rect button(box.x + box.width - w - 6.0f, box.y + 6.0f, w, mLineHeight + 4.0f);
    if (mUi.smallButton("Copy", button))
        copy(entry.kind == ChatEntry::Kind::Tool ? entry.arguments + "\n\n" + entry.text : entry.text);
    return ig::contains(button, mUi.pointerContentPosition());
}

void ChatWindow::drawTranscript(float top, float width, float height)
{
    const ig::Theme& theme = mUi.theme();
    mUi.setCursor(ig::Vec2(0.0f, top));
    if (!mUi.beginChild("transcript", height, true, width))
        return;
    const float viewWidth = width - theme.padding * 2.0f;
    const float viewHeight = height - theme.padding * 2.0f;
    const float barWidth = theme.scrollbarWidth * 0.65f;
    const float textWidth = viewWidth - barWidth - theme.itemSpacing;

    std::vector<ChatEntry>& entries = mSession.entries();
    mLayouts.resize(entries.size());
    float total = 0.0f;
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        relayout(mLayouts[i], entries[i], textWidth);
        total += mLayouts[i].height + (i + 1 < entries.size() ? kEntryGap : 0.0f);
    }
    const float maxScroll = std::max(0.0f, total - viewHeight);

    const bool modal = mSettings.isOpen() || mConfirmOpen;
    const ig::Vec2 pointer = mUi.pointerContentPosition();
    const ig::Rect viewRect(0.0f, 0.0f, viewWidth, viewHeight);
    if (!modal && mWheel != 0.0f && ig::contains(viewRect, pointer))
    {
        mScroll -= mWheel * mLineHeight * 3.0f;
        mStick = mScroll >= maxScroll - 0.5f;
    }
    if (!modal && !mUi.wantsTextInput())
    {
        if (mUi.isKeyPressed(ig::KeyCode::PageUp))
        {
            mScroll -= viewHeight * 0.9f;
            mStick = false;
        }
        if (mUi.isKeyPressed(ig::KeyCode::PageDown))
        {
            mScroll += viewHeight * 0.9f;
            mStick = mScroll >= maxScroll - 0.5f;
        }
    }

    const ig::Rect bar(viewWidth - barWidth, 0.0f, barWidth, viewHeight);
    const float thumbHeight = maxScroll > 0.0f
                                  ? std::max(theme.scrollbarMinThumb, viewHeight * viewHeight / total)
                                  : viewHeight;
    const float travel = viewHeight - thumbHeight;
    const bool down = mUi.isPointerButtonDown(ig::PointerButton::Left);
    if (!down)
    {
        mDragging = false;
    }
    else if (!mDragging && !modal && maxScroll > 0.0f &&
             ig::contains(bar, mUi.pointerPressedContentPosition(ig::PointerButton::Left)) && ig::contains(bar, pointer))
    {
        const float thumbTop = travel * std::clamp(mScroll / maxScroll, 0.0f, 1.0f);
        mDragging = true;
        mDragOffset = pointer.y >= thumbTop && pointer.y < thumbTop + thumbHeight ? pointer.y - thumbTop
                                                                                 : thumbHeight * 0.5f;
    }
    if (mDragging && travel > 0.0f)
    {
        mScroll = std::clamp((pointer.y - mDragOffset) / travel, 0.0f, 1.0f) * maxScroll;
        mStick = mScroll >= maxScroll - 0.5f;
    }
    if (mStick)
        mScroll = maxScroll;
    mScroll = std::clamp(mScroll, 0.0f, maxScroll);

    if (entries.empty())
    {
        static const std::string hint = "Ask something. The model can use the tools of the connected MCP servers.";
        const float hintWidth = mUi.textWidth(view(hint));
        mUi.drawText(view(hint), ig::Vec2(std::max(0.0f, (textWidth - hintWidth) * 0.5f), viewHeight * 0.45f), kDim);
    }
    float y = -mScroll;
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const float entryHeight = mLayouts[i].height;
        if (y + entryHeight >= 0.0f && y <= viewHeight)
            drawEntry(i, entries[i], mLayouts[i], y, textWidth, 0.0f, viewHeight);
        y += entryHeight + kEntryGap;
        if (y > viewHeight)
            break;
    }

    if (maxScroll > 0.0f)
    {
        const float thumbTop = travel * mScroll / maxScroll;
        mUi.drawRectFilled(bar, theme.inputBg);
        mUi.drawRectFilledRounded(ig::Rect(bar.x, thumbTop, bar.width, thumbHeight), barWidth * 0.5f,
                                  mDragging ? theme.sliderThumbPressed : theme.scrollbarThumb);
    }
    mUi.endChild();
}

void ChatWindow::drawInput(float top, float width, float height)
{
    const ig::Theme& theme = mUi.theme();
    const bool busy = mSession.busy();
    const float buttonWidth = mUi.textWidth("Stop") + theme.padding * 8.0f;
    mUi.inputTextMultiline("input", mInput, ig::Rect(0.0f, top, width - buttonWidth - theme.itemSpacing, height));
    const ig::Rect button(width - buttonWidth, top, buttonWidth, height);
    if (busy)
    {
        if (mUi.button("Stop", button))
            mSession.stop();
        mSubmit = false;
        return;
    }
    if (mUi.button("Send", button))
        mSubmit = true;
    if (mSubmit)
        sendInput();
}

void ChatWindow::sendInput()
{
    mSubmit = false;
    if (mSettings.isOpen() || mConfirmOpen || mSession.busy())
        return;
    const std::string text(mInput.data(), mInput.size());
    if (trim(text).empty())
        return;
    mSession.send(text);
    mInput.clear();
    mStick = true;
}

void ChatWindow::drawConfirmation()
{
    std::string name, arguments;
    const bool pending = mSession.pendingConfirmation(name, arguments);
    const ig::Theme& theme = mUi.theme();
    const ig::Vec2 size(std::min(640.0f, mDisplay.x - 24.0f), std::min(420.0f, mDisplay.y - 24.0f));
    if (!pending)
    {
        // A closed dialog is still begun: that is what releases iGUI's modal hold on the window.
        mConfirmOpen = false;
        mConfirmName.clear();
        mUi.beginDialog("Run this tool?", mConfirmOpen, size);
        return;
    }
    if (mConfirmName.empty())
    {
        mConfirmName = name;
        mConfirmOpen = true;
        mConfirmLines.clear();
    }

    bool answered = false;
    bool allow = false;
    if (mUi.beginDialog("Run this tool?", mConfirmOpen, size))
    {
        const float left = theme.windowPadding;
        const float top = theme.windowPadding * 0.5f;
        const float width = mUi.availableWidth() - left * 2.0f;
        const float height = mUi.availableHeight() - top - left;
        const float row = theme.widgetHeight;
        static const std::string asks = "The model wants to run";
        mUi.drawText(view(asks), ig::Vec2(left, top), theme.labelText);
        mUi.drawText(view(name), ig::Vec2(left + mUi.textWidth(view(asks)) + 8.0f, top), kToolName);
        const float listTop = top + mLineHeight + theme.itemSpacing;
        const float listHeight = top + height - listTop - row - theme.itemSpacing;
        if (mConfirmLines.empty())
        {
            const float wrapWidth = width - theme.padding * 2.0f - theme.scrollbarWidth;
            for (Line& line : wrap(arguments, wrapWidth, Format::Plain))
                mConfirmLines.push_back(std::move(line.text));
        }
        mUi.setCursor(ig::Vec2(left, listTop));
        if (listHeight > mLineHeight && mUi.beginChild("arguments", listHeight, true, width))
        {
            for (const std::string& line : mConfirmLines)
                mUi.label(view(line));
            mUi.endChild();
        }
        const float buttonsTop = top + height - row;
        const float buttonWidth = 110.0f;
        if (mUi.button("Allow", ig::Rect(left + width - buttonWidth, buttonsTop, buttonWidth, row)))
        {
            answered = true;
            allow = true;
        }
        if (mUi.button("Deny",
                       ig::Rect(left + width - buttonWidth * 2.0f - theme.itemSpacing, buttonsTop, buttonWidth, row)))
            answered = true;
        mUi.endDialog();
    }
    if (!mConfirmOpen)
        answered = true;
    if (answered)
    {
        mSession.answerConfirmation(allow);
        mConfirmOpen = false;
    }
}

void ChatWindow::saveConversation()
{
    const std::filesystem::path folder = mSession.configPath().parent_path() / "conversations";
    std::error_code code;
    std::filesystem::create_directories(folder, code);
    const std::filesystem::path path = folder / ("chat-" + timestamp() + ".json");
    std::string error;
    if (code)
        error = code.message();
    else if (mSession.saveConversation(path, error))
    {
        const std::string saved = "Saved to " + path.u8string();
        mUi.showToast("save", view(saved));
        return;
    }
    mUi.showToast("save", view("Not saved: " + error));
}

void ChatWindow::chooseProfile(int index)
{
    Config updated = mSession.config();
    if (index < 0 || index >= static_cast<int>(updated.profiles.size()))
        return;
    updated.currentProfile = updated.profiles[static_cast<std::size_t>(index)].name;
    std::string error;
    if (!mSession.apply(updated, error))
        mSession.notify(ChatEntry::Kind::Error, "Could not save the configuration: " + error);
}

void ChatWindow::copy(const std::string& text)
{
    mBackend.setClipboardText(view(text));
    mUi.showToast("copy", "Copied to the clipboard");
}

} // namespace mcpchat
