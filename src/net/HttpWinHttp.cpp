#include "net/Http.h"

#include "net/Url.h"
#include "util/Strings.h"

#include <windows.h>
#include <winhttp.h>

#include <memory>
#include <mutex>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

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
std::wstring wide(const std::string& text)
{
    if (text.empty())
        return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::string narrow(const std::wstring& text)
{
    if (text.empty())
        return std::string();
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::string describe(DWORD code, const std::string& url)
{
    switch (code)
    {
    case ERROR_WINHTTP_TIMEOUT:
        return "no answer from " + url + " in time";
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return "cannot connect to " + url;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return "cannot resolve the host of " + url;
    case ERROR_WINHTTP_SECURE_FAILURE:
        return "TLS failure talking to " + url;
    case ERROR_WINHTTP_OPERATION_CANCELLED:
        return "cancelled";
    default:
        return "WinHTTP error " + std::to_string(code) + " talking to " + url;
    }
}

struct Handle
{
    HINTERNET handle = nullptr;
    ~Handle()
    {
        if (handle)
            WinHttpCloseHandle(handle);
    }
};

// The request handle, closable once from either the worker or the Stop button.
struct RequestHandle
{
    HINTERNET handle = nullptr;
    std::mutex mutex;

    void close()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (handle)
        {
            WinHttpCloseHandle(handle);
            handle = nullptr;
        }
    }
};
} // namespace

bool send(const Request& request, Response& response, std::string& error, const BodySink& sink, CancelToken* cancel)
{
    response = Response();
    error.clear();
    if (cancel && cancel->isSet())
    {
        error = "cancelled";
        return false;
    }
    Url url;
    if (!Url::parse(request.url, url))
    {
        error = "not an http(s) URL: " + request.url;
        return false;
    }

    Handle session;
    session.handle = WinHttpOpen(L"mcpchat", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.handle)
        session.handle = WinHttpOpen(L"mcpchat", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.handle)
    {
        error = describe(GetLastError(), request.url);
        return false;
    }
    const int connectMs = static_cast<int>(request.connectTimeoutSeconds * 1000.0);
    const int receiveMs = static_cast<int>(request.timeoutSeconds * 1000.0);
    WinHttpSetTimeouts(session.handle, connectMs, connectMs, receiveMs, receiveMs);

    Handle connection;
    connection.handle = WinHttpConnect(session.handle, wide(url.host).c_str(), static_cast<INTERNET_PORT>(url.port), 0);
    if (!connection.handle)
    {
        error = describe(GetLastError(), request.url);
        return false;
    }

    RequestHandle handle;
    handle.handle = WinHttpOpenRequest(connection.handle, wide(request.method).c_str(), wide(url.path).c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       url.scheme == "https" ? WINHTTP_FLAG_SECURE : 0);
    if (!handle.handle)
    {
        error = describe(GetLastError(), request.url);
        return false;
    }
    std::unique_ptr<CancelToken::Registration> stop;
    if (cancel)
        stop = std::make_unique<CancelToken::Registration>(cancel->onCancel([&handle] { handle.close(); }));

    auto fail = [&](DWORD code)
    {
        error = cancel && cancel->isSet() ? std::string("cancelled") : describe(code, request.url);
        handle.close();
        return false;
    };

    std::wstring headers;
    for (const Header& header : request.headers)
        headers += wide(header.name + ": " + header.value) + L"\r\n";
    {
        std::lock_guard<std::mutex> lock(handle.mutex);
        if (!handle.handle)
            return fail(ERROR_WINHTTP_OPERATION_CANCELLED);
        if (!headers.empty())
            WinHttpAddRequestHeaders(handle.handle, headers.c_str(), static_cast<DWORD>(headers.size()),
                                     WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    // The handle is not held under the lock while blocked: Stop must be able to close it.
    HINTERNET live = handle.handle;
    if (!WinHttpSendRequest(live, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data()),
                            static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()), 0) ||
        !WinHttpReceiveResponse(live, nullptr))
        return fail(GetLastError());

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(live, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<long>(status);

    DWORD rawSize = 0;
    WinHttpQueryHeaders(live, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                        &rawSize, WINHTTP_NO_HEADER_INDEX);
    if (rawSize > 0)
    {
        std::wstring raw(rawSize / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(live, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                                &rawSize, WINHTTP_NO_HEADER_INDEX))
        {
            const std::string text = narrow(raw.substr(0, rawSize / sizeof(wchar_t)));
            std::size_t start = 0;
            while (start < text.size())
            {
                std::size_t end = text.find("\r\n", start);
                if (end == std::string::npos)
                    end = text.size();
                const std::string line = text.substr(start, end - start);
                const std::size_t colon = line.find(':');
                if (colon != std::string::npos && !startsWith(line, "HTTP/"))
                    response.headers.push_back({trim(line.substr(0, colon)), trim(line.substr(colon + 1))});
                start = end + 2;
            }
        }
    }

    std::string buffer;
    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(live, &available))
            return fail(GetLastError());
        if (available == 0)
            break;
        buffer.resize(available);
        DWORD read = 0;
        if (!WinHttpReadData(live, buffer.data(), available, &read))
            return fail(GetLastError());
        if (read == 0)
            break;
        if (sink)
        {
            if (!sink(response, std::string_view(buffer.data(), read)))
                break;
        }
        else
            response.body.append(buffer.data(), read);
    }
    handle.close();
    if (cancel && cancel->isSet())
    {
        error = "cancelled";
        return false;
    }
    return true;
}

} // namespace mcpchat::net
