#ifndef MCPCHAT_IMAGE_H
#define MCPCHAT_IMAGE_H

#include <cstdint>
#include <vector>

namespace mcpchat
{

// PNG, JPEG, BMP or GIF bytes as RGBA8 rows, top first. False when the bytes are none of those.
bool decodeImage(const std::vector<std::uint8_t>& bytes, std::vector<std::uint8_t>& rgba, int& width, int& height);

} // namespace mcpchat

#endif
