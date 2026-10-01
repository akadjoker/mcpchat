#include "render/GlBackend.h"

#include "render/GlFunctions.h"

#include <platform.h>

#include <cmath>

namespace mcpchat
{

namespace
{
const char* const kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
uniform vec2 uDisplaySize;
out vec2 vUv;
out vec4 vColor;
void main()
{
    vUv = aUv;
    vColor = aColor;
    gl_Position = vec4(aPosition.x / uDisplaySize.x * 2.0 - 1.0, 1.0 - aPosition.y / uDisplaySize.y * 2.0, 0.0, 1.0);
}
)";

const char* const kFragmentShader = R"(#version 330 core
in vec2 vUv;
in vec4 vColor;
uniform sampler2D uTexture;
out vec4 outColor;
void main()
{
    outColor = vColor * texture(uTexture, vUv);
}
)";

bool compile(gl::GLenum kind, const char* source, unsigned int& shader, std::string& error)
{
    shader = gl::CreateShader(kind);
    gl::ShaderSource(shader, 1, &source, nullptr);
    gl::CompileShader(shader);
    gl::GLint ok = 0;
    gl::GetShaderiv(shader, gl::COMPILE_STATUS, &ok);
    if (ok)
        return true;
    char log[1024] = {};
    gl::GetShaderInfoLog(shader, sizeof(log), nullptr, log);
    error = std::string("shader: ") + log;
    return false;
}

unsigned int makeTexture(const std::uint8_t* rgba, int width, int height)
{
    unsigned int texture = 0;
    gl::GenTextures(1, &texture);
    gl::BindTexture(gl::TEXTURE_2D, texture);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, width, height, 0, gl::RGBA, gl::UNSIGNED_BYTE, rgba);
    return texture;
}
} // namespace

GlBackend::GlBackend(PlatformWindow* window, float fontPixels)
    : mWindow(window), mFontAtlas(ig::defaultFontRanges(), 2048u, 1024u, fontPixels)
{
}

GlBackend::~GlBackend()
{
    if (!gl::DeleteProgram)
        return;
    gl::DeleteProgram(mProgram);
    gl::DeleteBuffers(1, &mVertexBuffer);
    gl::DeleteBuffers(1, &mIndexBuffer);
    gl::DeleteVertexArrays(1, &mVertexArray);
    gl::DeleteTextures(1, &mFontTexture);
    gl::DeleteTextures(1, &mWhiteTexture);
}

bool GlBackend::initialize(std::string& error)
{
    const char* missing = nullptr;
    if (!gl::load(missing))
    {
        error = std::string("OpenGL 3.3 function missing: ") + missing;
        return false;
    }
    unsigned int vertex = 0, fragment = 0;
    if (!compile(gl::VERTEX_SHADER, kVertexShader, vertex, error) ||
        !compile(gl::FRAGMENT_SHADER, kFragmentShader, fragment, error))
        return false;
    mProgram = gl::CreateProgram();
    gl::AttachShader(mProgram, vertex);
    gl::AttachShader(mProgram, fragment);
    gl::LinkProgram(mProgram);
    gl::DeleteShader(vertex);
    gl::DeleteShader(fragment);
    gl::GLint linked = 0;
    gl::GetProgramiv(mProgram, gl::LINK_STATUS, &linked);
    if (!linked)
    {
        char log[1024] = {};
        gl::GetProgramInfoLog(mProgram, sizeof(log), nullptr, log);
        error = std::string("shader link: ") + log;
        return false;
    }
    mDisplaySizeLocation = gl::GetUniformLocation(mProgram, "uDisplaySize");
    mTextureLocation = gl::GetUniformLocation(mProgram, "uTexture");

    gl::GenVertexArrays(1, &mVertexArray);
    gl::GenBuffers(1, &mVertexBuffer);
    gl::GenBuffers(1, &mIndexBuffer);
    gl::BindVertexArray(mVertexArray);
    gl::BindBuffer(gl::ARRAY_BUFFER, mVertexBuffer);
    gl::BindBuffer(gl::ELEMENT_ARRAY_BUFFER, mIndexBuffer);
    const gl::GLsizei stride = sizeof(ig::DrawVertex);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 2, gl::FLOAT, 0, stride, reinterpret_cast<const void*>(offsetof(ig::DrawVertex, position)));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 2, gl::FLOAT, 0, stride, reinterpret_cast<const void*>(offsetof(ig::DrawVertex, uv)));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 4, gl::UNSIGNED_BYTE, 1, stride, reinterpret_cast<const void*>(offsetof(ig::DrawVertex, color)));
    gl::BindVertexArray(0);

    if (!mFontAtlas.valid())
    {
        error = "the font atlas could not be built";
        return false;
    }
    // The atlas is coverage only: white, with the coverage as alpha, so vertex colours tint it.
    const ig::Span<const std::uint8_t> alpha = mFontAtlas.pixels();
    std::vector<std::uint8_t> rgba(alpha.size() * 4u, 255u);
    for (std::size_t i = 0; i < alpha.size(); ++i)
        rgba[i * 4u + 3u] = alpha[i];
    mFontTexture = makeTexture(rgba.data(), static_cast<int>(mFontAtlas.width()), static_cast<int>(mFontAtlas.height()));
    mFontAtlas.setTexture(ig::TextureId(mFontTexture));
    const std::uint8_t white[4] = {255, 255, 255, 255};
    mWhiteTexture = makeTexture(white, 1, 1);
    return true;
}

ig::TextMetrics GlBackend::measureText(ig::FontId font, ig::StringView text, float logicalSize, float)
{
    return mFontAtlas.measureText(font, text, logicalSize);
}

ig::String GlBackend::clipboardText()
{
    const char* text = clipboard_get();
    return text ? ig::String(text) : ig::String();
}

bool GlBackend::setClipboardText(ig::StringView text)
{
    const ig::String copy(text.data(), text.size());
    clipboard_set(copy.c_str());
    return true;
}

ig::TextureId GlBackend::createTexture(const std::uint8_t* rgba, int width, int height)
{
    return ig::TextureId(makeTexture(rgba, width, height));
}

void GlBackend::destroyTexture(ig::TextureId texture)
{
    const unsigned int name = static_cast<unsigned int>(texture.value);
    if (name)
        gl::DeleteTextures(1, &name);
}

void GlBackend::beginFrame(float red, float green, float blue)
{
    int width = 0, height = 0;
    window_get_framebuffer_size(mWindow, &width, &height);
    gl::Viewport(0, 0, width, height);
    gl::ClearColor(red, green, blue, 1.0f);
    gl::Clear(gl::COLOR_BUFFER_BIT);
}

bool GlBackend::render(const ig::DrawData& data)
{
    int framebufferWidth = 0, framebufferHeight = 0;
    window_get_framebuffer_size(mWindow, &framebufferWidth, &framebufferHeight);
    if (framebufferWidth <= 0 || framebufferHeight <= 0 || data.displaySize.x <= 0.0f || data.displaySize.y <= 0.0f)
        return true;
    const float scaleX = static_cast<float>(framebufferWidth) / data.displaySize.x;
    const float scaleY = static_cast<float>(framebufferHeight) / data.displaySize.y;

    gl::Enable(gl::BLEND);
    gl::BlendEquation(gl::FUNC_ADD);
    gl::BlendFuncSeparate(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA, gl::ONE, gl::ONE_MINUS_SRC_ALPHA);
    gl::Disable(gl::CULL_FACE);
    gl::Disable(gl::DEPTH_TEST);
    gl::Enable(gl::SCISSOR_TEST);
    gl::UseProgram(mProgram);
    gl::Uniform2f(mDisplaySizeLocation, data.displaySize.x, data.displaySize.y);
    gl::Uniform1i(mTextureLocation, 0);
    gl::ActiveTexture(gl::TEXTURE0);
    gl::BindVertexArray(mVertexArray);
    gl::BindBuffer(gl::ARRAY_BUFFER, mVertexBuffer);
    gl::BufferData(gl::ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(data.vertices.size() * sizeof(ig::DrawVertex)),
                   data.vertices.data(), gl::STREAM_DRAW);
    gl::BindBuffer(gl::ELEMENT_ARRAY_BUFFER, mIndexBuffer);
    gl::BufferData(gl::ELEMENT_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(data.indices.size() * sizeof(ig::DrawIndex)),
                   data.indices.data(), gl::STREAM_DRAW);

    for (std::size_t i = 0; i < data.commands.size(); ++i)
    {
        const ig::DrawCommand& command = data.commands[i];
        if (command.type != ig::DrawCommandType::Geometry)
            continue;
        const ig::GeometryCommand& geometry = command.payload.geometry;
        if (geometry.indexCount == 0 || geometry.firstIndex > data.indices.size() ||
            geometry.indexCount > data.indices.size() - geometry.firstIndex)
            continue;
        // GL's scissor origin is the bottom left, in framebuffer pixels.
        const int left = static_cast<int>(std::floor(geometry.clip.x * scaleX));
        const int top = static_cast<int>(std::floor(geometry.clip.y * scaleY));
        const int right = static_cast<int>(std::ceil((geometry.clip.x + geometry.clip.width) * scaleX));
        const int bottom = static_cast<int>(std::ceil((geometry.clip.y + geometry.clip.height) * scaleY));
        if (right <= left || bottom <= top)
            continue;
        gl::Scissor(left, framebufferHeight - bottom, right - left, bottom - top);
        const unsigned int texture = geometry.texture.value ? static_cast<unsigned int>(geometry.texture.value) : mWhiteTexture;
        gl::BindTexture(gl::TEXTURE_2D, texture);
        gl::DrawElementsBaseVertex(gl::TRIANGLES, static_cast<gl::GLsizei>(geometry.indexCount), gl::UNSIGNED_INT,
                                   reinterpret_cast<const void*>(static_cast<std::uintptr_t>(geometry.firstIndex) *
                                                                 sizeof(ig::DrawIndex)),
                                   geometry.vertexOffset);
    }
    gl::Disable(gl::SCISSOR_TEST);
    gl::BindVertexArray(0);
    return true;
}

} // namespace mcpchat
