#include "util/Base64.h"

namespace mcpchat
{

namespace
{
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int valueOf(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}
} // namespace

std::string base64Encode(const std::uint8_t* data, std::size_t size)
{
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3)
    {
        const std::uint32_t a = data[i];
        const std::uint32_t b = i + 1 < size ? data[i + 1] : 0;
        const std::uint32_t c = i + 2 < size ? data[i + 2] : 0;
        const std::uint32_t triple = (a << 16) | (b << 8) | c;
        out += kAlphabet[(triple >> 18) & 63];
        out += kAlphabet[(triple >> 12) & 63];
        out += i + 1 < size ? kAlphabet[(triple >> 6) & 63] : '=';
        out += i + 2 < size ? kAlphabet[triple & 63] : '=';
    }
    return out;
}

bool base64Decode(std::string_view text, std::vector<std::uint8_t>& out)
{
    out.clear();
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    int padding = 0;
    for (const char c : text)
    {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        if (c == '=')
        {
            ++padding;
            continue;
        }
        const int value = valueOf(c);
        if (value < 0 || padding > 0)
            return false;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return padding <= 2 && bits < 6;
}

} // namespace mcpchat
