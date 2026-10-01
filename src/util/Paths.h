#ifndef MCPCHAT_PATHS_H
#define MCPCHAT_PATHS_H

#include <filesystem>
#include <string>

namespace mcpchat
{

// %APPDATA%/mcpchat, ~/Library/Application Support/mcpchat or $XDG_CONFIG_HOME/mcpchat (~/.config/mcpchat).
std::filesystem::path configDirectory();

// The variable's value as UTF-8, empty when unset.
std::string environment(const std::string& name);

bool readTextFile(const std::filesystem::path& path, std::string& out);
// Through a temporary file and a rename, so a crash never leaves half a file.
bool writeTextFileAtomic(const std::filesystem::path& path, const std::string& text, std::string* error = nullptr);

} // namespace mcpchat

#endif
