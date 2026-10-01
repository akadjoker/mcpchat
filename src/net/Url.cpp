#include "net/Url.h"

#include "util/Strings.h"

#include <cstdlib>

namespace mcpchat::net
{

bool Url::parse(const std::string& text, Url& out)
{
    const std::string url = trim(text);
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos)
        return false;
    out.scheme = toLower(url.substr(0, schemeEnd));
    if (out.scheme != "http" && out.scheme != "https")
        return false;
    const std::size_t hostStart = schemeEnd + 3;
    const std::size_t pathStart = url.find_first_of("/?#", hostStart);
    std::string authority = url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos)
        authority = authority.substr(at + 1);
    out.port = out.scheme == "https" ? 443 : 80;
    if (!authority.empty() && authority[0] == '[')
    {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos)
            return false;
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':')
            out.port = std::atoi(authority.c_str() + close + 2);
    }
    else
    {
        const std::size_t colon = authority.rfind(':');
        out.host = authority.substr(0, colon);
        if (colon != std::string::npos)
            out.port = std::atoi(authority.c_str() + colon + 1);
    }
    out.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    if (!out.path.empty() && out.path[0] != '/')
        out.path = "/" + out.path;
    return !out.host.empty() && out.port > 0 && out.port < 65536;
}

std::string Url::authority() const
{
    const bool standard = (scheme == "https" && port == 443) || (scheme == "http" && port == 80);
    const std::string name = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    return standard ? name : name + ":" + std::to_string(port);
}

} // namespace mcpchat::net
