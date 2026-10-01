#include "ui/TextWrap.h"

namespace mcpchat
{

namespace
{
std::size_t nextCharacter(std::string_view text, std::size_t at)
{
    ++at;
    while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
        ++at;
    return at;
}

void wrapParagraph(std::string_view paragraph, float width, const std::function<float(std::string_view)>& measure,
                   std::vector<std::string>& lines)
{
    if (paragraph.empty())
    {
        lines.emplace_back();
        return;
    }
    std::size_t start = 0;
    while (start < paragraph.size())
    {
        // The longest run of whole characters that fits; at least one, so a narrow width still advances.
        std::size_t fit = start;
        std::size_t lastSpace = std::string_view::npos;
        float used = 0.0f;
        while (fit < paragraph.size())
        {
            const std::size_t next = nextCharacter(paragraph, fit);
            const float advance = measure(paragraph.substr(fit, next - fit));
            if (used + advance > width && fit > start)
                break;
            if (paragraph[fit] == ' ')
                lastSpace = fit;
            used += advance;
            fit = next;
        }
        if (fit >= paragraph.size())
        {
            lines.emplace_back(paragraph.substr(start));
            break;
        }
        const std::size_t end = lastSpace != std::string_view::npos && lastSpace > start ? lastSpace : fit;
        lines.emplace_back(paragraph.substr(start, end - start));
        start = end;
        while (start < paragraph.size() && paragraph[start] == ' ')
            ++start;
    }
}
} // namespace

std::vector<std::string> wrapText(std::string_view text, float width,
                                  const std::function<float(std::string_view)>& measure)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t end = text.find('\n', start);
        std::string_view paragraph = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!paragraph.empty() && paragraph.back() == '\r')
            paragraph.remove_suffix(1);
        wrapParagraph(paragraph, width, measure, lines);
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return lines;
}

} // namespace mcpchat
