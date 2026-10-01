#include "net/Http.h"

#include "util/Strings.h"

#include <curl/curl.h>

#include <atomic>
#include <mutex>

namespace mcpchat::net
{

std::string Response::header(std::string_view name) const
{
    const std::string wanted = toLower(name);
    for (const Header& entry : headers)
    {
        if (toLower(entry.name) == wanted)
            return entry.value;
    }
    return std::string();
}

namespace
{
std::once_flag gCurlInit;

struct Transfer
{
    Response* response = nullptr;
    const BodySink* sink = nullptr;
    CancelToken* cancel = nullptr;
    std::atomic<bool> aborted{false};
    bool sinkStopped = false;
};

size_t onHeader(char* data, size_t size, size_t count, void* user)
{
    Transfer& transfer = *static_cast<Transfer*>(user);
    const std::string line = trim(std::string_view(data, size * count));
    if (startsWith(line, "HTTP/"))
    {
        // A new status line (after a redirect or a 100 Continue) starts a fresh header set.
        transfer.response->headers.clear();
        return size * count;
    }
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos)
        transfer.response->headers.push_back({trim(line.substr(0, colon)), trim(line.substr(colon + 1))});
    return size * count;
}

size_t onBody(char* data, size_t size, size_t count, void* user)
{
    Transfer& transfer = *static_cast<Transfer*>(user);
    const size_t bytes = size * count;
    if (transfer.sink && *transfer.sink)
    {
        if (!(*transfer.sink)(*transfer.response, std::string_view(data, bytes)))
        {
            transfer.sinkStopped = true;
            return 0;
        }
        return bytes;
    }
    transfer.response->body.append(data, bytes);
    return bytes;
}

int onProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const Transfer& transfer = *static_cast<Transfer*>(user);
    return transfer.aborted.load() || (transfer.cancel && transfer.cancel->isSet()) ? 1 : 0;
}
} // namespace

bool send(const Request& request, Response& response, std::string& error, const BodySink& sink, CancelToken* cancel)
{
    std::call_once(gCurlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    response = Response();
    error.clear();
    if (cancel && cancel->isSet())
    {
        error = "cancelled";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl)
    {
        error = "cannot start an HTTP transfer";
        return false;
    }
    Transfer transfer;
    transfer.response = &response;
    transfer.sink = &sink;
    transfer.cancel = cancel;

    curl_slist* headers = nullptr;
    for (const Header& header : request.headers)
        headers = curl_slist_append(headers, (header.name + ": " + header.value).c_str());
    // libcurl would otherwise add "Expect: 100-continue" to large POSTs and wait for it.
    headers = curl_slist_append(headers, "Expect:");

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(request.connectTimeoutSeconds * 1000.0));
    // A stream may be long; what matters is that bytes keep coming.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>(request.timeoutSeconds));
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    if (request.method == "POST")
    {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }
    else if (request.method == "GET")
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    else
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());

    // The progress callback runs at least once a second, so Stop is seen within that.
    const CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code == CURLE_OK || (code == CURLE_WRITE_ERROR && transfer.sinkStopped))
        return true;
    if (cancel && cancel->isSet())
        error = "cancelled";
    else if (code == CURLE_OPERATION_TIMEDOUT)
        error = "no answer from " + request.url + " in time";
    else
        error = curl_easy_strerror(code);
    return false;
}

} // namespace mcpchat::net
