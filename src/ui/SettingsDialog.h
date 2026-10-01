#ifndef MCPCHAT_SETTINGS_DIALOG_H
#define MCPCHAT_SETTINGS_DIALOG_H

#include "config/Config.h"

#include <igui/Gui.hpp>

#include <string>
#include <vector>

namespace mcpchat
{

class ChatSession;

// Edits a copy of the configuration; Save hands it to the session, which writes the file and reconnects if needed.
class SettingsDialog
{
public:
    void open(const Config& config);
    bool isOpen() const
    {
        return mOpen;
    }
    // `display` is the window size, which the dialog never outgrows.
    void draw(ig::Context& ui, ChatSession& session, const ig::Vec2& display);

private:
    // The multi-line fields keep the user's text as typed and are parsed on Save.
    struct ServerText
    {
        ig::String args;
        ig::String env;
        ig::String headers;
    };

    void drawProfiles(ig::Context& ui, ChatSession& session, float top, float width, float height);
    void drawServers(ig::Context& ui, ChatSession& session, float top, float width, float height);
    void drawGeneral(ig::Context& ui, ChatSession& session, float top, float width);
    bool commit(std::string& error);

    Config mDraft;
    std::vector<ServerText> mServerText;
    bool mOpen = false;
    float mLeft = 0.0f;
    int mTab = 0;
    int mProfile = 0;
    int mServer = 0;
    ig::String mKey;
    ig::String mSystemPrompt;
    int mPromptProfile = -1;
    std::string mMessage;
};

} // namespace mcpchat

#endif
