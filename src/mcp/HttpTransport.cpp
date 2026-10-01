#include "mcp/HttpTransport.h"

#include "net/Sse.h"
#include "util/Strings.h"

namespace mcpchat
{

namespace
{
std::string snippet(const std::string& body)
{
    return std::string(utf8Prefix(body, 200));
}

Json parse(const std::string& body)
{
    Json value = Json::parse(body, nullptr, false);
    if (value.is_discarded())
        throw TransportError("the server answered with something that is not JSON: " + snippet(body));
    return value;
}
} // namespace

HttpTransport::HttpTransport(std::string url, std::vector<net::Header> headers, double timeoutSeconds)
    : mUrl(std::move(url)), mHeaders(std::move(headers)), mTimeout(timeoutSeconds)
{
}

void HttpTransport::setProtocolVersion(const std::string& version)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mProtocolVersion = version;
}

std::optional<Json> HttpTransport::answerFromEventStream(const std::string& body, const Json& id)
{
    std::optional<Json> answer;
    net::SseParser parser(
        [&](const std::string& data)
        {
            const Json event = parse(data);
            if (event.is_object() && event.contains("id") && event["id"] == id &&
                (event.contains("result") || event.contains("error")))
            {
                answer = event;
                return false;
            }
            return true;
        });
    if (parser.feed(body))
        parser.finish();
    return answer;
}

std::optional<Json> HttpTransport::send(const Json& message, CancelToken* cancel)
{
    net::Request request;
    request.method = "POST";
    request.url = mUrl;
    request.timeoutSeconds = mTimeout;
    request.body = dump(message);
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json, text/event-stream"}};
    request.headers.insert(request.headers.end(), mHeaders.begin(), mHeaders.end());
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mSessionId.empty())
            request.headers.push_back({"Mcp-Session-Id", mSessionId});
        if (!mProtocolVersion.empty())
            request.headers.push_back({"MCP-Protocol-Version", mProtocolVersion});
    }

    net::Response response;
    std::string error;
    if (!net::send(request, response, error, net::BodySink(), cancel))
    {
        if (error == "cancelled")
            throw Cancelled();
        throw TransportError("cannot reach " + mUrl + ": " + error);
    }
    const std::string session = response.header("Mcp-Session-Id");
    if (!session.empty())
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSessionId = session;
    }

    if (response.status >= 400)
    {
        const Json reply = Json::parse(response.body, nullptr, false);
        if (!reply.is_discarded() && reply.is_object() && reply.value("jsonrpc", "") == "2.0" && reply.contains("error"))
            return reply;
        bool hadSession = false;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            hadSession = !mSessionId.empty();
        }
        if (response.status == 404 && hadSession)
            throw TransportError("the server ended the session; reconnect", 404);
        const std::string hint = response.status == 401 || response.status == 403 ? " (check the token)" : "";
        throw TransportError(mUrl + " answered HTTP " + std::to_string(response.status) + hint + ": " +
                                 snippet(response.body),
                             response.status);
    }
    if (response.status == 202)
        return std::nullopt;
    const std::string contentType = response.header("Content-Type");
    const std::string kind = toLower(trim(contentType.substr(0, contentType.find(';'))));
    if (kind == "text/event-stream")
    {
        if (!message.contains("id"))
            return std::nullopt;
        std::optional<Json> answer = answerFromEventStream(response.body, message["id"]);
        if (!answer)
            throw TransportError("the event stream ended without an answer");
        return answer;
    }
    if (trim(response.body).empty())
        return std::nullopt;
    return parse(response.body);
}

void HttpTransport::close()
{
    std::string session;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        session.swap(mSessionId);
    }
    if (session.empty())
        return;
    net::Request request;
    request.method = "DELETE";
    request.url = mUrl;
    request.timeoutSeconds = 5.0;
    request.connectTimeoutSeconds = 5.0;
    request.headers = mHeaders;
    request.headers.push_back({"Mcp-Session-Id", session});
    net::Response response;
    std::string error;
    // The server may not allow ending sessions; there is nothing more to do either way.
    net::send(request, response, error);
}

} // namespace mcpchat
