#include "config/Config.h"
#include "render/GlBackend.h"
#include "ui/ChatSession.h"
#include "ui/ChatWindow.h"
#include "util/Version.h"

#include <platform.h>

#include <cstdio>
#include <string>
#include <vector>

#ifndef _WIN32
#include <csignal>
#endif

namespace
{
constexpr float kFontSize = 16.0f;
constexpr double kFrameSeconds = 1.0 / 60.0;
constexpr double kIdleFrameSeconds = 1.0 / 20.0;

int encodeUtf8(std::uint32_t codepoint, char* out)
{
    if (codepoint < 0x80)
    {
        out[0] = static_cast<char>(codepoint);
        return 1;
    }
    if (codepoint < 0x800)
    {
        out[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        out[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
        return 0;
    if (codepoint < 0x10000)
    {
        out[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        out[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 3;
    }
    if (codepoint < 0x110000)
    {
        out[0] = static_cast<char>(0xF0 | (codepoint >> 18));
        out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 4;
    }
    return 0;
}

ig::KeyCode toKeyCode(int key)
{
    switch (key)
    {
    case KEY_BACKSPACE:
        return ig::KeyCode::Backspace;
    case KEY_ENTER:
    case KEY_KP_ENTER:
        return ig::KeyCode::Enter;
    case KEY_DELETE:
        return ig::KeyCode::Delete;
    case KEY_TAB:
        return ig::KeyCode::Tab;
    case KEY_LEFT:
        return ig::KeyCode::Left;
    case KEY_RIGHT:
        return ig::KeyCode::Right;
    case KEY_UP:
        return ig::KeyCode::Up;
    case KEY_DOWN:
        return ig::KeyCode::Down;
    case KEY_HOME:
        return ig::KeyCode::Home;
    case KEY_END:
        return ig::KeyCode::End;
    case KEY_PAGE_UP:
        return ig::KeyCode::PageUp;
    case KEY_PAGE_DOWN:
        return ig::KeyCode::PageDown;
    case KEY_ESCAPE:
        return ig::KeyCode::Escape;
    case KEY_A:
        return ig::KeyCode::A;
    case KEY_C:
        return ig::KeyCode::C;
    case KEY_D:
        return ig::KeyCode::D;
    case KEY_F:
        return ig::KeyCode::F;
    case KEY_H:
        return ig::KeyCode::H;
    case KEY_N:
        return ig::KeyCode::N;
    case KEY_O:
        return ig::KeyCode::O;
    case KEY_S:
        return ig::KeyCode::S;
    case KEY_V:
        return ig::KeyCode::V;
    case KEY_X:
        return ig::KeyCode::X;
    case KEY_Y:
        return ig::KeyCode::Y;
    case KEY_Z:
        return ig::KeyCode::Z;
    case KEY_F4:
        return ig::KeyCode::F4;
    case KEY_F5:
        return ig::KeyCode::F5;
    default:
        return ig::KeyCode::None;
    }
}

bool toPointerButton(int button, ig::PointerButton& out)
{
    switch (button)
    {
    case MOUSE_LEFT:
        out = ig::PointerButton::Left;
        return true;
    case MOUSE_RIGHT:
        out = ig::PointerButton::Right;
        return true;
    case MOUSE_MIDDLE:
        out = ig::PointerButton::Middle;
        return true;
    default:
        return false;
    }
}

void translate(const Event& event, PlatformWindow* window, ig::Context& ui, mcpchat::ChatWindow& chat)
{
    switch (event.type)
    {
    case EVENT_MOUSE_MOVE:
        ui.pushEvent(ig::Event::pointerMove(static_cast<float>(event.data.mouse.x),
                                            static_cast<float>(event.data.mouse.y)));
        break;
    case EVENT_MOUSE_BUTTON:
    {
        ig::PointerButton button;
        if (!toPointerButton(event.data.mouse.button, button))
            break;
        const float x = static_cast<float>(event.data.mouse.x);
        const float y = static_cast<float>(event.data.mouse.y);
        ui.pushEvent(event.data.mouse.down ? ig::Event::pointerDown(button, x, y) : ig::Event::pointerUp(button, x, y));
        break;
    }
    case EVENT_MOUSE_WHEEL:
    {
        ig::Event wheel;
        wheel.type = ig::EventType::PointerWheel;
        wheel.wheelX = event.data.wheel.x;
        wheel.wheelY = event.data.wheel.y;
        wheel.control = (key_mods(window) & KEYMOD_CTRL) != 0;
        ui.pushEvent(wheel);
        chat.wheel(event.data.wheel.y);
        break;
    }
    case EVENT_KEY:
    {
        if (!event.data.key.down)
            break;
        const ig::KeyCode key = toKeyCode(event.data.key.key);
        if (key == ig::KeyCode::None)
            break;
        const bool control = (event.data.key.mods & KEYMOD_CTRL) != 0;
        const bool shift = (event.data.key.mods & KEYMOD_SHIFT) != 0;
        if (key == ig::KeyCode::Enter && control)
        {
            chat.submit();
            break;
        }
        ui.pushEvent(ig::Event::keyDown(key, control, shift));
        break;
    }
    case EVENT_WINDOW_FOCUS:
        if (!event.data.focus.gained)
            ui.pushEvent(ig::Event::focusLost());
        break;
    default:
        break;
    }
}

// Hands the frame's platform events to iGUI. True when there was any, so an idle window can slow down.
// iGUI sees one frame's events at once, so a press and release in the same frame would never make a click: from a
// release of a button pressed this frame on, the events wait for the next frame, in order.
bool forwardEvents(PlatformWindow* window, ig::Context& ui, mcpchat::ChatWindow& chat, std::vector<Event>& deferred)
{
    std::vector<Event> events;
    events.swap(deferred);
    Event event;
    while (poll_event(window, &event))
    {
        if (event.type != EVENT_WINDOW_DROP)
            events.push_back(event);
    }
    bool any = !events.empty();
    bool pressed[MOUSE_BUTTON_MAX] = {};
    for (std::size_t i = 0; i < events.size(); ++i)
    {
        const Event& current = events[i];
        if (current.type == EVENT_MOUSE_BUTTON && current.data.mouse.button >= 0 &&
            current.data.mouse.button < MOUSE_BUTTON_MAX)
        {
            if (!current.data.mouse.down && pressed[current.data.mouse.button])
            {
                deferred.assign(events.begin() + static_cast<std::ptrdiff_t>(i), events.end());
                break;
            }
            if (current.data.mouse.down)
                pressed[current.data.mouse.button] = true;
        }
        translate(current, window, ui, chat);
    }
    // Text comes through its own queue, not as events, so it carries the keyboard layout and composed input.
    const bool control = (key_mods(window) & KEYMOD_CTRL) != 0 && (key_mods(window) & KEYMOD_ALT) == 0;
    while (const std::uint32_t codepoint = char_get_pressed(window))
    {
        any = true;
        if (codepoint < 0x20 || codepoint == 0x7F || control)
            continue;
        char text[4];
        const int length = encodeUtf8(codepoint, text);
        if (length > 0)
            ui.pushEvent(ig::Event::textInput(ig::StringView(text, static_cast<std::size_t>(length))));
    }
    return any;
}
} // namespace

int main()
{
#ifndef _WIN32
    // A stdio server that dies must not take the window with it through a write to its closed pipe.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    if (!platform_init())
    {
        std::fprintf(stderr, "mcpchat: %s\n", platform_get_error());
        return 1;
    }

    WindowConfig config = {};
    const std::string title = mcpchat::versionText();
    config.title = title.c_str();
    config.width = 1100;
    config.height = 780;
    config.x = WINDOW_POS_CENTERED;
    config.y = WINDOW_POS_CENTERED;
    config.monitor = MONITOR_CURRENT;
    config.mode = WINDOW_WINDOWED;
    config.render = RENDER_GL;
    config.gl.profile = GL_PROFILE_CORE;
    config.gl.major = 3;
    config.gl.minor = 3;
    config.resizable = true;
    config.vsync = true;
    PlatformWindow* window = window_create(&config);
    if (!window)
    {
        std::fprintf(stderr, "mcpchat: %s\n", platform_get_error());
        platform_shutdown();
        return 1;
    }
    window_set_size_limits(window, 640, 420, 0, 0);

    int status = 0;
    {
        const float scale = window_content_scale(window);
        mcpchat::GlBackend backend(window, kFontSize * scale);
        std::string error;
        if (!backend.initialize(error))
        {
            std::fprintf(stderr, "mcpchat: %s\n", error.c_str());
            status = 1;
        }
        else
        {
            ig::Context ui(backend, &backend.fontAtlas());
            ig::Theme theme = ig::makeTheme(ig::ThemePreset::Dark);
            theme.font = backend.fontAtlas().defaultFont();
            theme.fontSize = kFontSize;
            ui.setTheme(theme);

            mcpchat::ChatSession session(mcpchat::defaultConfigPath());
            if (session.load(error))
                session.connect();
            else
                session.notify(mcpchat::ChatEntry::Kind::Error,
                               "Could not read " + session.configPath().u8string() + ": " + error);
            mcpchat::ChatWindow chat(session, backend, ui);

            std::vector<Event> deferred;
            double last = time_seconds();
            double quietSince = last;
            while (!window_should_close(window))
            {
                window_begin_frame(window);
                const bool input = forwardEvents(window, ui, chat, deferred) || !deferred.empty();
                const bool changed = session.pump();
                for (const std::uint64_t texture : session.takeReleasedTextures())
                    backend.destroyTexture(ig::TextureId(texture));

                const double now = time_seconds();
                if (input || changed || session.busy())
                    quietSince = now;
                int width = 0, height = 0;
                window_get_size(window, &width, &height);
                if (width <= 0 || height <= 0 || window_is_minimized(window))
                {
                    time_sleep(50);
                    continue;
                }

                ui.beginFrame(ig::FrameInfo(static_cast<float>(width), static_cast<float>(height), scale,
                                            static_cast<float>(now - last)));
                last = now;
                chat.draw();
                const ig::DrawData& data = ui.endFrame();
                const ig::Color& background = theme.windowBackground;
                backend.beginFrame(background.r / 255.0f, background.g / 255.0f, background.b / 255.0f);
                backend.render(data);
                window_swap(window);

                // Vsync paces the loop where the driver honours it; this caps it where it does not, lower when idle.
                const double budget = now - quietSince > 1.0 ? kIdleFrameSeconds : kFrameSeconds;
                const double spent = time_seconds() - now;
                if (spent < budget)
                    time_sleep(static_cast<std::uint32_t>((budget - spent) * 1000.0));
            }
        }
    }
    window_destroy(window);
    platform_shutdown();
    return status;
}
