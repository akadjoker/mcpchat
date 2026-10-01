#include "net/Sse.h"

namespace mcpchat::net
{

bool SseParser::feed(std::string_view chunk)
{
    if (mStopped)
        return false;
    mPartial.append(chunk.data(), chunk.size());
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t end = mPartial.find('\n', start);
        if (end == std::string::npos)
            break;
        std::string_view text(mPartial.data() + start, end - start);
        if (!text.empty() && text.back() == '\r')
            text.remove_suffix(1);
        start = end + 1;
        if (!line(text))
        {
            mStopped = true;
            mPartial.clear();
            return false;
        }
    }
    mPartial.erase(0, start);
    return true;
}

bool SseParser::finish()
{
    if (mStopped)
        return false;
    if (!mPartial.empty())
    {
        std::string rest;
        rest.swap(mPartial);
        if (!line(rest))
            return false;
    }
    return line(std::string_view());
}

bool SseParser::line(std::string_view text)
{
    if (text.empty())
    {
        if (!mHasData)
            return true;
        std::string data;
        data.swap(mData);
        mHasData = false;
        return mHandler(data);
    }
    if (text.compare(0, 5, "data:") != 0)
        return true;
    text.remove_prefix(5);
    if (!text.empty() && text[0] == ' ')
        text.remove_prefix(1);
    if (mHasData)
        mData += '\n';
    mData.append(text.data(), text.size());
    mHasData = true;
    return true;
}

} // namespace mcpchat::net
