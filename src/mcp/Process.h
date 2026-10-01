#ifndef MCPCHAT_PROCESS_H
#define MCPCHAT_PROCESS_H

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mcpchat
{

// A child process with its stdin, stdout and stderr on pipes.
class Process
{
public:
    // `environment` is added to (and overrides) this process's own. Null with `error` set when it cannot start.
    static std::unique_ptr<Process> start(const std::string& command, const std::vector<std::string>& arguments,
                                          const std::map<std::string, std::string>& environment, std::string& error);
    ~Process();

    bool write(std::string_view data);
    // Blocking; 0 at the end of the stream or on error.
    std::size_t readOutput(char* buffer, std::size_t size);
    std::size_t readError(char* buffer, std::size_t size);
    void closeInput();

    // Closes stdin, waits up to `seconds` for it to leave, then kills it. Returns the exit code (-1 if unknown).
    int stop(double seconds);
    // The exit code once it has exited, else nothing (returns false).
    bool exited(int& code);

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

private:
    Process() = default;
    struct Native;
    std::unique_ptr<Native> mNative;
};

} // namespace mcpchat

#endif
