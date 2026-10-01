#include "util/Paths.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mcpchat
{

std::string environment(const std::string& name)
{
#ifdef _WIN32
    const int wideLength = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    std::wstring wideName(static_cast<std::size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wideName.data(), wideLength);
    const DWORD size = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (size == 0)
        return std::string();
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(wideName.c_str(), value.data(), size);
    value.resize(written);
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), length, nullptr,
                        nullptr);
    return out;
#else
    const char* value = std::getenv(name.c_str());
    return value ? std::string(value) : std::string();
#endif
}

std::filesystem::path configDirectory()
{
#ifdef _WIN32
    std::string base = environment("APPDATA");
    std::filesystem::path root = base.empty() ? std::filesystem::u8path(environment("USERPROFILE")) / "AppData" / "Roaming"
                                              : std::filesystem::u8path(base);
#elif defined(__APPLE__)
    std::filesystem::path root = std::filesystem::path(environment("HOME")) / "Library" / "Application Support";
#else
    const std::string xdg = environment("XDG_CONFIG_HOME");
    std::filesystem::path root = xdg.empty() ? std::filesystem::path(environment("HOME")) / ".config"
                                             : std::filesystem::path(xdg);
#endif
    return root / "mcpchat";
}

bool readTextFile(const std::filesystem::path& path, std::string& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

bool writeTextFileAtomic(const std::filesystem::path& path, const std::string& text, std::string* error)
{
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(text.data(), static_cast<std::streamsize>(text.size())))
        {
            if (error)
                *error = "cannot write " + temporary.u8string();
            return false;
        }
    }
    std::filesystem::rename(temporary, path, code);
    if (code)
    {
        if (error)
            *error = "cannot replace " + path.u8string() + ": " + code.message();
        return false;
    }
    return true;
}

} // namespace mcpchat
