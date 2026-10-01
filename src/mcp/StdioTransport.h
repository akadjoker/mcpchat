#ifndef MCPCHAT_STDIO_TRANSPORT_H
#define MCPCHAT_STDIO_TRANSPORT_H

#include "mcp/McpClient.h"
#include "mcp/Process.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace mcpchat
{

// stdio: the server is a child process; one JSON-RPC message per line on its stdin and stdout.
class StdioTransport : public Transport
{
public:
    // Throws TransportError when the process cannot start.
    StdioTransport(const std::string& command, const std::vector<std::string>& arguments,
                   const std::map<std::string, std::string>& environment, double timeoutSeconds);
    ~StdioTransport() override;

    std::optional<Json> send(const Json& message, CancelToken* cancel) override;
    void close() override;

    // The last lines the server wrote to stderr.
    std::string stderrTail(std::size_t lines = 5) const;

private:
    void readOutput();
    void readErrors();
    void dispatch(const Json& message);
    bool write(const Json& message);
    std::string exitMessage();

    std::string mCommand;
    double mTimeout;
    std::unique_ptr<Process> mProcess;
    std::thread mOutputThread;
    std::thread mErrorThread;

    mutable std::mutex mMutex;
    std::condition_variable mArrived;
    std::map<std::string, Json> mReplies; // by dumped id
    std::map<std::string, bool> mWaiting;
    std::deque<std::string> mStderr;
    bool mOutputClosed = false;
    std::mutex mWriteMutex;
};

} // namespace mcpchat

#endif
