#ifndef MCPCHAT_GL_FUNCTIONS_H
#define MCPCHAT_GL_FUNCTIONS_H

#include <cstddef>
#include <cstdint>

// The OpenGL 3.3 core functions the renderer calls, loaded through zen_platform's gl_proc_address.
namespace gl
{

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLfloat = float;
using GLchar = char;
using GLbitfield = unsigned int;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

constexpr GLenum TRIANGLES = 0x0004;
constexpr GLenum UNSIGNED_BYTE = 0x1401;
constexpr GLenum UNSIGNED_INT = 0x1405;
constexpr GLenum FLOAT = 0x1406;
constexpr GLenum BLEND = 0x0BE2;
constexpr GLenum SCISSOR_TEST = 0x0C11;
constexpr GLenum DEPTH_TEST = 0x0B71;
constexpr GLenum CULL_FACE = 0x0B44;
constexpr GLenum SRC_ALPHA = 0x0302;
constexpr GLenum ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum ONE = 1;
constexpr GLenum FUNC_ADD = 0x8006;
constexpr GLenum TEXTURE_2D = 0x0DE1;
constexpr GLenum TEXTURE0 = 0x84C0;
constexpr GLenum TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum TEXTURE_WRAP_S = 0x2802;
constexpr GLenum TEXTURE_WRAP_T = 0x2803;
constexpr GLenum LINEAR = 0x2601;
constexpr GLenum CLAMP_TO_EDGE = 0x812F;
constexpr GLenum RGBA = 0x1908;
constexpr GLenum RGBA8 = 0x8058;
constexpr GLenum UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLenum ARRAY_BUFFER = 0x8892;
constexpr GLenum ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum STREAM_DRAW = 0x88E0;
constexpr GLenum FRAGMENT_SHADER = 0x8B30;
constexpr GLenum VERTEX_SHADER = 0x8B31;
constexpr GLenum COMPILE_STATUS = 0x8B81;
constexpr GLenum LINK_STATUS = 0x8B82;
constexpr GLenum COLOR_BUFFER_BIT = 0x4000;

#define MCPCHAT_GL_FUNCTIONS(X)                                                                                        \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                                                \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei))                                                                 \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                                          \
    X(void, Clear, (GLbitfield))                                                                                       \
    X(void, Enable, (GLenum))                                                                                          \
    X(void, Disable, (GLenum))                                                                                         \
    X(void, BlendEquation, (GLenum))                                                                                   \
    X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                                       \
    X(void, PixelStorei, (GLenum, GLint))                                                                              \
    X(void, GenTextures, (GLsizei, GLuint*))                                                                           \
    X(void, DeleteTextures, (GLsizei, const GLuint*))                                                                  \
    X(void, BindTexture, (GLenum, GLuint))                                                                             \
    X(void, ActiveTexture, (GLenum))                                                                                   \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                                                                    \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))                  \
    X(void, GenBuffers, (GLsizei, GLuint*))                                                                            \
    X(void, DeleteBuffers, (GLsizei, const GLuint*))                                                                   \
    X(void, BindBuffer, (GLenum, GLuint))                                                                              \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                                     \
    X(void, GenVertexArrays, (GLsizei, GLuint*))                                                                       \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint*))                                                              \
    X(void, BindVertexArray, (GLuint))                                                                                 \
    X(void, EnableVertexAttribArray, (GLuint))                                                                         \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))                             \
    X(GLuint, CreateShader, (GLenum))                                                                                  \
    X(void, DeleteShader, (GLuint))                                                                                    \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                                       \
    X(void, CompileShader, (GLuint))                                                                                   \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                                                     \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                                    \
    X(GLuint, CreateProgram, ())                                                                                       \
    X(void, DeleteProgram, (GLuint))                                                                                   \
    X(void, AttachShader, (GLuint, GLuint))                                                                            \
    X(void, LinkProgram, (GLuint))                                                                                     \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                                                    \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                                   \
    X(void, UseProgram, (GLuint))                                                                                      \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*))                                                              \
    X(void, Uniform1i, (GLint, GLint))                                                                                 \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat))                                                                      \
    X(void, DrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void*, GLint))

#if defined(_WIN32)
#define MCPCHAT_GL_API __stdcall
#else
#define MCPCHAT_GL_API
#endif

#define MCPCHAT_GL_DECLARE(ret, name, args) extern ret(MCPCHAT_GL_API* name) args;
MCPCHAT_GL_FUNCTIONS(MCPCHAT_GL_DECLARE)
#undef MCPCHAT_GL_DECLARE

// False with the name of the first missing function in `missing`.
bool load(const char*& missing);

} // namespace gl

#endif
