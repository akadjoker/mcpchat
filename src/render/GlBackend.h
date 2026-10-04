#ifndef MCPCHAT_GL_BACKEND_H
#define MCPCHAT_GL_BACKEND_H

#include <igui/Backend.hpp>
#include <igui/FontAtlas.hpp>

#include <cstdint>
#include <string>
#include <vector>

struct PlatformWindow;

namespace mcpchat
{

// iGUI over OpenGL 3.3 core on a zen_platform window: one dynamic vertex and index buffer, one shader, the font atlas
// and any RGBA image as textures. Text is drawn as atlas quads by the Context, so only geometry reaches render().
class GlBackend : public ig::Backend
{
public:
    // The font is baked at `fontPixels`, the text size times the display scale, so glyphs land on whole pixels.
    GlBackend(PlatformWindow* window, float fontPixels);
    ~GlBackend() override;

    // Loads GL, compiles the shader and uploads the font atlas. False with `error` set.
    bool initialize(std::string& error);

    ig::FontAtlas& fontAtlas()
    {
        return mFontAtlas;
    }

    PlatformWindow* window() const
    {
        return mWindow;
    }

    ig::TextMetrics measureText(ig::FontId font, ig::StringView text, float logicalSize, float dpiScale) override;
    bool render(const ig::DrawData& data) override;
    ig::String clipboardText() override;
    bool setClipboardText(ig::StringView text) override;

    // An RGBA8 image as a texture for ig::Context::image(); release it with destroyTexture().
    ig::TextureId createTexture(const std::uint8_t* rgba, int width, int height);
    void destroyTexture(ig::TextureId texture);

    void beginFrame(float red, float green, float blue);

private:
    PlatformWindow* mWindow;
    ig::FontAtlas mFontAtlas;
    unsigned int mProgram = 0;
    unsigned int mVertexArray = 0;
    unsigned int mVertexBuffer = 0;
    unsigned int mIndexBuffer = 0;
    unsigned int mFontTexture = 0;
    unsigned int mWhiteTexture = 0;
    int mDisplaySizeLocation = -1;
    int mTextureLocation = -1;
};

} // namespace mcpchat

#endif
