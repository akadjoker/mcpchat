#include "render/Image.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include <stb/stb_image.h>

#include <cstring>

namespace mcpchat
{

bool decodeImage(const std::vector<std::uint8_t>& bytes, std::vector<std::uint8_t>& rgba, int& width, int& height)
{
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (!pixels)
        return false;
    rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    std::memcpy(rgba.data(), pixels, rgba.size());
    stbi_image_free(pixels);
    return true;
}

} // namespace mcpchat
