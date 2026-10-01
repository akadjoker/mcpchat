#include "LoopbackServer.h"

#include <chrono>
#include <cctype>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
static void closeSocket(SocketHandle s)
{
    closesocket(s);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static void closeSocket(SocketHandle s)
{
    close(s);
}
#endif

namespace
{
#ifdef MSG_NOSIGNAL
constexpr int kNoSignal = MSG_NOSIGNAL;
#else
constexpr int kNoSignal = 0;
#endif

bool sendAll(SocketHandle socket, const std::string& data)
{
    std::size_t sent = 0;
    while (sent < data.size())
    {
        const int got = static_cast<int>(::send(socket, data.data() + sent, static_cast<int>(data.size() - sent), kNoSignal));
        if (got <= 0)
            return false;
        sent += static_cast<std::size_t>(got);
    }
    return true;
}

const char* reason(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 404:
        return "Not Found";
    default:
        return "Status";
    }
}
} // namespace

LoopbackServer::LoopbackServer(Handler handler) : mHandler(std::move(handler))
{
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    SocketHandle listener = ::socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    listen(listener, 16);
    socklen_t length = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    mPort = ntohs(address.sin_port);
    mSocket = static_cast<long long>(listener);
    mThread = std::thread([this] { serve(); });
}

LoopbackServer::~LoopbackServer()
{
    stop();
}

std::string LoopbackServer::url(const std::string& path) const
{
    return "http://127.0.0.1:" + std::to_string(mPort) + path;
}

void LoopbackServer::stop()
{
    if (!mRunning.exchange(false))
        return;
    // Wakes the accept() with a connection of our own, then closes the listener.
    SocketHandle poke = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(mPort));
    connect(poke, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    closeSocket(poke);
    if (mThread.joinable())
        mThread.join();
    closeSocket(static_cast<SocketHandle>(mSocket));
}

std::vector<LoopbackServer::Request> LoopbackServer::requests() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mRequests;
}

void LoopbackServer::serve()
{
    while (mRunning)
    {
        SocketHandle client = accept(static_cast<SocketHandle>(mSocket), nullptr, nullptr);
        if (!mRunning)
        {
            closeSocket(client);
            break;
        }
        std::string data;
        char buffer[8192];
        std::size_t headerEnd = std::string::npos;
        while (headerEnd == std::string::npos)
        {
            const int got = static_cast<int>(recv(client, buffer, sizeof(buffer), 0));
            if (got <= 0)
                break;
            data.append(buffer, static_cast<std::size_t>(got));
            headerEnd = data.find("\r\n\r\n");
        }
        if (headerEnd == std::string::npos)
        {
            closeSocket(client);
            continue;
        }
        Request request;
        const std::string head = data.substr(0, headerEnd);
        std::size_t lineEnd = head.find("\r\n");
        const std::string first = head.substr(0, lineEnd);
        request.method = first.substr(0, first.find(' '));
        request.path = first.substr(first.find(' ') + 1, first.rfind(' ') - first.find(' ') - 1);
        while (lineEnd != std::string::npos)
        {
            const std::size_t start = lineEnd + 2;
            lineEnd = head.find("\r\n", start);
            const std::string line = head.substr(start, lineEnd == std::string::npos ? std::string::npos : lineEnd - start);
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            std::string name = line.substr(0, colon);
            for (char& c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ')
                value.erase(0, 1);
            request.headers[name] = value;
        }
        const std::size_t length =
            request.headers.count("content-length") ? std::stoul(request.headers["content-length"]) : 0;
        request.body = data.substr(headerEnd + 4);
        while (request.body.size() < length)
        {
            const int got = static_cast<int>(recv(client, buffer, sizeof(buffer), 0));
            if (got <= 0)
                break;
            request.body.append(buffer, static_cast<std::size_t>(got));
        }
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mRequests.push_back(request);
        }
        const Reply reply = mHandler(request);
        std::string response = "HTTP/1.1 " + std::to_string(reply.status) + " " + reason(reply.status) + "\r\n";
        for (const auto& header : reply.headers)
            response += header.first + ": " + header.second + "\r\n";
        if (reply.chunks.size() <= 1)
            response += "Content-Length: " + std::to_string(reply.chunks.empty() ? 0 : reply.chunks[0].size()) + "\r\n";
        response += "Connection: close\r\n\r\n";
        bool alive = sendAll(client, response);
        for (std::size_t i = 0; alive && i < reply.chunks.size(); ++i)
        {
            if (i > 0 && reply.pauseMs > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(reply.pauseMs));
            alive = sendAll(client, reply.chunks[i]);
        }
#ifdef _WIN32
        shutdown(client, SD_SEND);
#else
        shutdown(client, SHUT_WR);
#endif
        closeSocket(client);
    }
}
