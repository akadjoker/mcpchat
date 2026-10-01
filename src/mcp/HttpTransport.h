#ifndef MCPCHAT_HTTP_TRANSPORT_H
#define MCPCHAT_HTTP_TRANSPORT_H

#include "mcp/McpClient.h"
#include "net/Http.h"

#include <mutex>

namespace mcpchat
{

// Streamable HTTP: each message is a POST; the answer comes back as one JSON body or as an event stream.
class HttpTransport : public Transport
{
public:
    HttpTransport(std::string url, std::vector<net::Header> headers, double timeoutSeconds);

    std::optional<Json> send(const Json& message, CancelToken* cancel) override;
    void setProtocolVersion(const std::string& version) override;
    // Ends the session on the server, when it gave one.
    void close() override;

    // The JSON-RPC answer to `id` out of an SSE body; server notifications on the way are skipped.
    static std::optional<Json> answerFromEventStream(const std::string& body, const Json& id);

private:
    std::string mUrl;
    std::vector<net::Header> mHeaders;
    double mTimeout;
    std::mutex mMutex;
    std::string mSessionId;
    std::string mProtocolVersion;
};

} // namespace mcpchat

#endif
