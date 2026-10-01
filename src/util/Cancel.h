#ifndef MCPCHAT_CANCEL_H
#define MCPCHAT_CANCEL_H

#include <atomic>
#include <functional>
#include <map>
#include <mutex>

namespace mcpchat
{

// The Stop button. A network call registers how to interrupt itself (close a handle, kill a process) for as long as
// it is blocked, so Stop takes effect at once instead of at the next timeout.
class CancelToken
{
public:
    bool isSet() const
    {
        return mSet.load();
    }

    void cancel()
    {
        std::map<int, std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mSet = true;
            callbacks = mCallbacks;
        }
        for (auto& entry : callbacks)
            entry.second();
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mSet = false;
    }

    class Registration
    {
    public:
        Registration(CancelToken* token, int id) : mToken(token), mId(id)
        {
        }
        Registration(Registration&& other) noexcept : mToken(other.mToken), mId(other.mId)
        {
            other.mToken = nullptr;
        }
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;
        Registration& operator=(Registration&&) = delete;
        ~Registration()
        {
            if (mToken)
                mToken->forget(mId);
        }

    private:
        CancelToken* mToken;
        int mId;
    };

    // `callback` runs when Stop is pressed while the registration lives, or at once if it already was.
    Registration onCancel(std::function<void()> callback)
    {
        bool already = false;
        int id = 0;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            already = mSet;
            if (!already)
            {
                id = ++mNextId;
                mCallbacks.emplace(id, callback);
            }
        }
        if (already)
            callback();
        return Registration(already ? nullptr : this, id);
    }

private:
    void forget(int id)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCallbacks.erase(id);
    }

    std::atomic<bool> mSet{false};
    std::mutex mMutex;
    std::map<int, std::function<void()>> mCallbacks;
    int mNextId = 0;
};

struct Cancelled
{
};

} // namespace mcpchat

#endif
