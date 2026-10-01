#ifndef MCPCHAT_URL_H
#define MCPCHAT_URL_H

#include <string>

namespace mcpchat::net
{

struct Url
{
    std::string scheme; // "http" or "https"
    std::string host;
    int port = 0;
    std::string path; // starts with '/', includes any query

    // False unless `text` is an absolute http(s) URL with a host.
    static bool parse(const std::string& text, Url& out);
    std::string authority() const;
};

} // namespace mcpchat::net

#endif
