#ifndef MCPCHAT_VERSION_H
#define MCPCHAT_VERSION_H

#include <string>

// MCPCHAT_VERSION and MCPCHAT_VENDOR come from CMake; the fallbacks keep the header usable on its own.
#ifndef MCPCHAT_VERSION
#define MCPCHAT_VERSION "0.0.0"
#endif
#ifndef MCPCHAT_VENDOR
#define MCPCHAT_VENDOR "DjokerSoft"
#endif

namespace mcpchat
{

// The brand and version, as shown in the window title and the about line.
inline std::string versionText()
{
    return std::string(MCPCHAT_VENDOR) + " mcpchat " MCPCHAT_VERSION;
}

} // namespace mcpchat

#endif
