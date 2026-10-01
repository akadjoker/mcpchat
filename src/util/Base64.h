#ifndef MCPCHAT_BASE64_H
#define MCPCHAT_BASE64_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mcpchat
{

std::string base64Encode(const std::uint8_t* data, std::size_t size);
// False on any character outside the alphabet (whitespace is skipped) or bad padding.
bool base64Decode(std::string_view text, std::vector<std::uint8_t>& out);

} // namespace mcpchat

#endif
