#ifndef MCPCHAT_STRINGS_H
#define MCPCHAT_STRINGS_H

#include <string>
#include <string_view>

namespace mcpchat
{

std::string trim(std::string_view text);
bool startsWith(std::string_view text, std::string_view prefix);
bool endsWith(std::string_view text, std::string_view suffix);
std::string toLower(std::string_view text);

// The longest prefix of at most `bytes` bytes that does not cut a UTF-8 sequence.
std::string_view utf8Prefix(std::string_view text, std::size_t bytes);

// `text` cut to `limit` bytes with a note of how much was dropped.
std::string truncate(const std::string& text, std::size_t limit);

} // namespace mcpchat

#endif
