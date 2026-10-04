#ifndef MCPCHAT_CHAT_WINDOW_H
#define MCPCHAT_CHAT_WINDOW_H

#include "ui/ChatSession.h"
#include "ui/SettingsDialog.h"

#include <igui/Gui.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mcpchat
{

class GlBackend;

// The whole window: a top bar, the conversation, the input box and the dialogs.
class ChatWindow
{
public:
    ChatWindow(ChatSession& session, GlBackend& backend, ig::Context& ui);

    // Wheel steps from the platform, taken before iGUI's own child region swallows them.
    void wheel(float steps);
    // Ctrl+Enter.
    void submit();
    // Ctrl+V with an image and no text on the clipboard: keeps it for the next message. False when the clipboard
    // holds text (iGUI's own paste then has it) or no image, so the key is left alone.
    bool attachFromClipboard();
    // Between ui.beginFrame() and ui.endFrame().
    void draw();

private:
    enum class LineStyle : std::uint8_t
    {
        Text,
        Heading,
        Code,
        Fence
    };

    enum class Format : std::uint8_t
    {
        Plain,
        Markdown,
        Code
    };

    struct Line
    {
        std::string text;
        LineStyle style = LineStyle::Text;
    };

    // An entry's wrapped lines, rebuilt only when what it shows or the width changes.
    struct Layout
    {
        ChatEntry::Kind kind = ChatEntry::Kind::Notice;
        std::size_t textSize = SIZE_MAX;
        std::size_t argumentsSize = 0;
        std::size_t images = 0;
        bool finished = false;
        bool expanded = false;
        float width = 0.0f;

        std::vector<Line> body;
        std::vector<Line> arguments;
        std::vector<float> imageHeights;
        float height = 0.0f;
    };

    void drawTopBar(float width);
    void drawTranscript(float top, float width, float height);
    void drawInput(float top, float width, float height);
    void drawConfirmation();

    void relayout(Layout& layout, ChatEntry& entry, float width);
    std::vector<Line> wrap(const std::string& text, float width, Format format) const;
    float drawEntry(std::size_t index, ChatEntry& entry, const Layout& layout, float top, float width, float viewTop,
                    float viewBottom);
    float drawLines(const std::vector<Line>& lines, float x, float top, float width, const ig::Color& color,
                    float viewTop, float viewBottom);
    // True while the pointer is over the button, which then owns the click.
    bool drawCopyButton(const ChatEntry& entry, const ig::Rect& box);
    void uploadImages(ChatEntry& entry);

    void sendInput();
    // "/attach <path>" and "/detach"; true when the line was one of them and must not be sent.
    bool handleCommand(const std::string& text);
    // Opens the file dialog and keeps the chosen image for the next message.
    void chooseAttachment();
    void attach(const std::string& path);
    // Tells the user and holds on to it for the next message.
    void keepAttachment(ChatAttachment attachment);
    void saveConversation();
    void chooseProfile(int index);
    void copy(const std::string& text);

    ChatSession& mSession;
    GlBackend& mBackend;
    ig::Context& mUi;
    SettingsDialog mSettings;

    float mLineHeight = 0.0f;
    ig::Vec2 mDisplay;
    std::vector<Layout> mLayouts;

    ig::String mInput;
    bool mSubmit = false;
    // The image waiting to go with the next message, if any.
    std::optional<ChatAttachment> mAttachment;

    float mScroll = 0.0f;
    float mWheel = 0.0f;
    bool mStick = true;
    bool mDragging = false;
    float mDragOffset = 0.0f;

    bool mConfirmOpen = false;
    std::string mConfirmName;
    std::vector<std::string> mConfirmLines;
};

} // namespace mcpchat

#endif
