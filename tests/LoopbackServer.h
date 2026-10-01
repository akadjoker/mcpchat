#ifndef MCPCHAT_LOOPBACK_SERVER_H
#define MCPCHAT_LOOPBACK_SERVER_H

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// A minimal HTTP/1.1 server on 127.0.0.1 for tests: one request per connection, the body sent in the reply's
// chunks with an optional pause between them, and the connection closed after it.
class LoopbackServer
{
public:
    struct Request
    {
        std::string method;
        std::string path;
        std::map<std::string, std::string> headers; // lower-case names
        std::string body;
    };

    struct Reply
    {
        int status = 200;
        std::vector<std::pair<std::string, std::string>> headers;
        std::vector<std::string> chunks;
        int pauseMs = 0;
    };

    using Handler = std::function<Reply(const Request&)>;

    explicit LoopbackServer(Handler handler);
    ~LoopbackServer();

    std::string url(const std::string& path) const;
    void stop();

    std::vector<Request> requests() const;

private:
    void serve();

    Handler mHandler;
    int mPort = 0;
    long long mSocket = -1;
    std::atomic<bool> mRunning{true};
    std::thread mThread;
    mutable std::mutex mMutex;
    std::vector<Request> mRequests;
};

#endif
