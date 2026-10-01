#include "render/GlFunctions.h"

#include <platform.h>

#include <string>

namespace gl
{

#define MCPCHAT_GL_DEFINE(ret, name, args) ret(MCPCHAT_GL_API* name) args = nullptr;
MCPCHAT_GL_FUNCTIONS(MCPCHAT_GL_DEFINE)
#undef MCPCHAT_GL_DEFINE

bool load(const char*& missing)
{
    missing = nullptr;
#define MCPCHAT_GL_LOAD(ret, name, args)                                                                               \
    name = reinterpret_cast<ret(MCPCHAT_GL_API*) args>(gl_proc_address("gl" #name));                                   \
    if (!name && !missing)                                                                                             \
        missing = "gl" #name;
    MCPCHAT_GL_FUNCTIONS(MCPCHAT_GL_LOAD)
#undef MCPCHAT_GL_LOAD
    return missing == nullptr;
}

} // namespace gl
