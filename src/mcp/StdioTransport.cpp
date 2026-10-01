#include "mcp/StdioTransport.h"

#include "util/Cancel.h"
#include "util/Strings.h"

#include <chrono>

namespace mcpchat
{

StdioTransport::StdioTransport(const std::string& command, const std::vector<std::string>& arguments,
                               const std::map<std::string, std::string>& environment, double timeoutSeconds)
    : mCommand(command), mTimeout(timeoutSeconds)
{
    std::string error;
    mProcess = Process::start(command, arguments, environment, error);
    if (!mProcess)
        throw TransportError(error);
    mOutputThread = std::thread([this] { readOutput(); });
    mErrorThread = std::thread([this] { readErrors(); });
}

StdioTransport::~StdioTransport()
{
    close();
}

void StdioTransport::close()
{
    if (!mProcess)
        return;
    mProcess->stop(3.0);
    if (mOutputThread.joinable())
        mOutputThread.join();
    if (mErrorThread.joinable())
        mErrorThread.join();
    mProcess.reset();
}

bool StdioTransport::write(const Json& message)
{
    std::lock_guard<std::mutex> lock(mWriteMutex);
    return mProcess && mProcess->write(dump(message) + "\n");
}

std::optional<Json> StdioTransport::send(const Json& message, CancelToken* cancel)
{
    const bool expectsReply = message.contains("id") && message.contains("method");
    const std::string key = expectsReply ? dump(message["id"]) : std::string();
    if (expectsReply)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mWaiting[key] = true;
    }
    if (!write(message))
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mWaiting.erase(key);
        throw TransportError(exitMessage());
    }
    if (!expectsReply)
        return std::nullopt;

    std::unique_ptr<CancelToken::Registration> stop;
    if (cancel)
        stop = std::make_unique<CancelToken::Registration>(cancel->onCancel(
            [this]
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mArrived.notify_all();
            }));

    std::unique_lock<std::mutex> lock(mMutex);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(mTimeout);
    const bool ready = mArrived.wait_until(lock, deadline,
                                           [&]
                                           {
                                               return mReplies.count(key) || mOutputClosed ||
                                                      (cancel && cancel->isSet());
                                           });
    mWaiting.erase(key);
    auto found = mReplies.find(key);
    if (found != mReplies.end())
    {
        Json reply = std::move(found->second);
        mReplies.erase(found);
        return reply;
    }
    lock.unlock();
    if (cancel && cancel->isSet())
        throw Cancelled();
    if (!ready)
        throw TransportError(mCommand + " did not answer " + message.value("method", std::string("a request")) +
                             " in " + std::to_string(static_cast<int>(mTimeout)) + " s");
    throw TransportError(exitMessage());
}

void StdioTransport::readOutput()
{
    std::string pending;
    char buffer[65536];
    for (;;)
    {
        const std::size_t got = mProcess->readOutput(buffer, sizeof(buffer));
        if (got == 0)
            break;
        pending.append(buffer, got);
        std::size_t start = 0;
        for (std::size_t end; (end = pending.find('\n', start)) != std::string::npos; start = end + 1)
        {
            const std::string line = trim(std::string_view(pending.data() + start, end - start));
            if (line.empty())
                continue;
            const Json message = Json::parse(line, nullptr, false);
            if (message.is_discarded())
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mStderr.push_back("(not JSON on stdout) " + std::string(utf8Prefix(line, 200)));
                if (mStderr.size() > 50)
                    mStderr.pop_front();
                continue;
            }
            if (message.is_array())
            {
                for (const Json& item : message)
                    dispatch(item);
            }
            else
                dispatch(message);
        }
        pending.erase(0, start);
    }
    std::lock_guard<std::mutex> lock(mMutex);
    mOutputClosed = true;
    mArrived.notify_all();
}

void StdioTransport::readErrors()
{
    std::string pending;
    char buffer[4096];
    for (;;)
    {
        const std::size_t got = mProcess->readError(buffer, sizeof(buffer));
        if (got == 0)
            break;
        pending.append(buffer, got);
        std::size_t start = 0;
        for (std::size_t end; (end = pending.find('\n', start)) != std::string::npos; start = end + 1)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mStderr.push_back(trim(std::string_view(pending.data() + start, end - start)));
            if (mStderr.size() > 50)
                mStderr.pop_front();
        }
        pending.erase(0, start);
    }
}

void StdioTransport::dispatch(const Json& message)
{
    if (!message.is_object())
        return;
    if (message.contains("method") && message.contains("id"))
    {
        // A request from the server: ping is answered, anything else this client does not offer.
        const std::string method = message["method"].is_string() ? message["method"].get<std::string>() : "";
        if (method == "ping")
            write({{"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", Json::object()}});
        else
            write({{"jsonrpc", "2.0"},
                   {"id", message["id"]},
                   {"error", {{"code", -32601}, {"message", "Method not found: " + method}}}});
        return;
    }
    if (!message.contains("id"))
        return;
    const std::string key = dump(message["id"]);
    std::lock_guard<std::mutex> lock(mMutex);
    if (mWaiting.count(key))
    {
        mReplies[key] = message;
        mArrived.notify_all();
    }
}

std::string StdioTransport::stderrTail(std::size_t lines) const
{
    std::lock_guard<std::mutex> lock(mMutex);
    std::string out;
    const std::size_t first = mStderr.size() > lines ? mStderr.size() - lines : 0;
    for (std::size_t i = first; i < mStderr.size(); ++i)
    {
        if (!out.empty())
            out += '\n';
        out += mStderr[i];
    }
    return out;
}

std::string StdioTransport::exitMessage()
{
    int code = 0;
    const bool exited = mProcess && mProcess->exited(code);
    std::string text = mCommand + (exited ? " exited with code " + std::to_string(code) : " closed its output");
    const std::string tail = stderrTail();
    if (!tail.empty())
        text += ":\n" + tail;
    return text;
}

} // namespace mcpchat
