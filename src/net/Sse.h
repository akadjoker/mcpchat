#ifndef MCPCHAT_SSE_H
#define MCPCHAT_SSE_H

#include <functional>
#include <string>
#include <string_view>

namespace mcpchat::net
{

// Server-sent events fed in arbitrary chunks. Each event's `data:` lines are joined with '\n' and handed over when
// the blank line ending the event arrives; comments, `event:` and `id:` lines are ignored.
class SseParser
{
public:
    // Returning false stops the stream.
    using EventHandler = std::function<bool(const std::string& data)>;

    explicit SseParser(EventHandler handler) : mHandler(std::move(handler))
    {
    }

    // False once the handler asked to stop.
    bool feed(std::string_view chunk);
    // Delivers an event the stream ended without its blank line.
    bool finish();

private:
    bool line(std::string_view text);

    EventHandler mHandler;
    std::string mPartial;
    std::string mData;
    bool mHasData = false;
    bool mStopped = false;
};

} // namespace mcpchat::net

#endif
