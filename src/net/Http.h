#ifndef MCPCHAT_HTTP_H
#define MCPCHAT_HTTP_H

#include "util/Cancel.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mcpchat::net
{

struct Header
{
    std::string name;
    std::string value;
};

struct Request
{
    std::string method = "POST";
    std::string url;
    std::vector<Header> headers;
    std::string body;
    double connectTimeoutSeconds = 10.0;
    // Longest silence while waiting for the answer.
    double timeoutSeconds = 300.0;
};

struct Response
{
    long status = 0;
    std::vector<Header> headers;
    std::string body;

    // Case-insensitive; empty when absent.
    std::string header(std::string_view name) const;
};

// Gets the body as it arrives, once the status and headers are known; returning false ends the transfer.
using BodySink = std::function<bool(const Response& head, std::string_view chunk)>;

// One HTTP(S) exchange: libcurl on Linux and macOS, WinHTTP on Windows. False with `error` set when nothing usable
// came back (no connection, timeout, TLS failure, Stop); an HTTP error status is a successful exchange. With a sink
// the body goes to it instead of Response::body. Stop interrupts a blocked read at once.
bool send(const Request& request, Response& response, std::string& error, const BodySink& sink = BodySink(),
          CancelToken* cancel = nullptr);

} // namespace mcpchat::net

#endif
