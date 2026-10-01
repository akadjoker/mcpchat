#include "mcp/Process.h"

#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace mcpchat
{

#ifdef _WIN32

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

// One argument quoted the way CommandLineToArgvW and the C runtime read it back.
std::wstring quote(const std::wstring& argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;
    std::wstring out = L"\"";
    for (std::size_t i = 0;; ++i)
    {
        std::size_t backslashes = 0;
        while (i < argument.size() && argument[i] == L'\\')
        {
            ++i;
            ++backslashes;
        }
        if (i == argument.size())
        {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (argument[i] == L'"')
            out.append(backslashes * 2 + 1, L'\\');
        else
            out.append(backslashes, L'\\');
        out += argument[i];
    }
    return out + L"\"";
}

std::wstring environmentBlock(const std::map<std::string, std::string>& extra)
{
    std::map<std::wstring, std::wstring> variables;
    LPWCH current = GetEnvironmentStringsW();
    for (LPWCH entry = current; entry && *entry; entry += wcslen(entry) + 1)
    {
        const std::wstring line(entry);
        const std::size_t equals = line.find(L'=', 1);
        if (equals != std::wstring::npos)
            variables[line.substr(0, equals)] = line.substr(equals + 1);
    }
    if (current)
        FreeEnvironmentStringsW(current);
    for (const auto& item : extra)
        variables[wide(item.first)] = wide(item.second);
    std::wstring block;
    for (const auto& item : variables)
        block += item.first + L"=" + item.second + L'\0';
    block += L'\0';
    return block;
}
} // namespace

struct Process::Native
{
    HANDLE process = nullptr;
    // Holds the child and everything it starts, so stopping it reaches grandchildren that share the pipes.
    HANDLE job = nullptr;
    HANDLE input = nullptr;
    HANDLE output = nullptr;
    HANDLE errors = nullptr;
};

std::unique_ptr<Process> Process::start(const std::string& command, const std::vector<std::string>& arguments,
                                        const std::map<std::string, std::string>& environment, std::string& error)
{
    SECURITY_ATTRIBUTES inherit = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childIn = nullptr, parentIn = nullptr, childOut = nullptr, parentOut = nullptr, childErr = nullptr,
           parentErr = nullptr;
    if (!CreatePipe(&childIn, &parentIn, &inherit, 0) || !CreatePipe(&parentOut, &childOut, &inherit, 0) ||
        !CreatePipe(&parentErr, &childErr, &inherit, 0))
    {
        error = "cannot create pipes for " + command;
        return nullptr;
    }
    SetHandleInformation(parentIn, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentOut, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentErr, HANDLE_FLAG_INHERIT, 0);

    std::wstring line = quote(wide(command));
    for (const std::string& argument : arguments)
        line += L" " + quote(wide(argument));
    std::wstring block = environmentBlock(environment);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childIn;
    startup.hStdOutput = childOut;
    startup.hStdError = childErr;
    PROCESS_INFORMATION info = {};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
    BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, flags, block.data(), nullptr,
                                  &startup, &info);
    if (!started && GetLastError() == ERROR_FILE_NOT_FOUND)
    {
        // npx, uvx and friends are .cmd scripts that only the shell runs.
        std::wstring shell = L"cmd.exe /d /s /c \"" + line + L"\"";
        started = CreateProcessW(nullptr, shell.data(), nullptr, nullptr, TRUE, flags, block.data(), nullptr,
                                 &startup, &info);
    }
    const DWORD code = started ? 0 : GetLastError();
    CloseHandle(childIn);
    CloseHandle(childOut);
    CloseHandle(childErr);
    if (!started)
    {
        CloseHandle(parentIn);
        CloseHandle(parentOut);
        CloseHandle(parentErr);
        error = "cannot start " + command + " (Windows error " + std::to_string(code) + ")";
        return nullptr;
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        if (!AssignProcessToJobObject(job, info.hProcess))
        {
            CloseHandle(job);
            job = nullptr;
        }
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    std::unique_ptr<Process> process(new Process());
    process->mNative = std::make_unique<Native>();
    process->mNative->process = info.hProcess;
    process->mNative->job = job;
    process->mNative->input = parentIn;
    process->mNative->output = parentOut;
    process->mNative->errors = parentErr;
    return process;
}

Process::~Process()
{
    if (!mNative)
        return;
    stop(0.0);
    if (mNative->output)
        CloseHandle(mNative->output);
    if (mNative->errors)
        CloseHandle(mNative->errors);
    if (mNative->process)
        CloseHandle(mNative->process);
    if (mNative->job)
        CloseHandle(mNative->job);
}

bool Process::write(std::string_view data)
{
    while (!data.empty())
    {
        if (!mNative->input)
            return false;
        DWORD written = 0;
        if (!WriteFile(mNative->input, data.data(), static_cast<DWORD>(data.size()), &written, nullptr))
            return false;
        data.remove_prefix(written);
    }
    return true;
}

std::size_t Process::readOutput(char* buffer, std::size_t size)
{
    DWORD read = 0;
    return ReadFile(mNative->output, buffer, static_cast<DWORD>(size), &read, nullptr) ? read : 0;
}

std::size_t Process::readError(char* buffer, std::size_t size)
{
    DWORD read = 0;
    return ReadFile(mNative->errors, buffer, static_cast<DWORD>(size), &read, nullptr) ? read : 0;
}

void Process::closeInput()
{
    if (mNative->input)
    {
        CloseHandle(mNative->input);
        mNative->input = nullptr;
    }
}

bool Process::exited(int& code)
{
    DWORD status = 0;
    if (!GetExitCodeProcess(mNative->process, &status) || status == STILL_ACTIVE)
        return false;
    code = static_cast<int>(status);
    return true;
}

int Process::stop(double seconds)
{
    closeInput();
    if (WaitForSingleObject(mNative->process, static_cast<DWORD>(seconds * 1000.0)) != WAIT_OBJECT_0)
        TerminateProcess(mNative->process, 1);
    // Whatever it started may still hold the pipes open.
    if (mNative->job)
        TerminateJobObject(mNative->job, 1);
    WaitForSingleObject(mNative->process, 3000);
    int code = -1;
    exited(code);
    return code;
}

#else

struct Process::Native
{
    pid_t pid = -1;
    int input = -1;
    int output = -1;
    int errors = -1;
    bool reaped = false;
    int status = 0;
};

std::unique_ptr<Process> Process::start(const std::string& command, const std::vector<std::string>& arguments,
                                        const std::map<std::string, std::string>& environment, std::string& error)
{
    // Writing to a server that died must fail the write, not kill this program.
    static const bool ignoringBrokenPipes = [] { return signal(SIGPIPE, SIG_IGN) != SIG_ERR; }();
    (void)ignoringBrokenPipes;
    int in[2], out[2], err[2];
    if (pipe(in) != 0 || pipe(out) != 0 || pipe(err) != 0)
    {
        error = std::string("cannot create pipes: ") + std::strerror(errno);
        return nullptr;
    }
    for (const int fd : {in[1], out[0], err[0]})
        fcntl(fd, F_SETFD, FD_CLOEXEC);

    std::map<std::string, std::string> variables;
    for (char** entry = environ; entry && *entry; ++entry)
    {
        const std::string line(*entry);
        const std::size_t equals = line.find('=');
        if (equals != std::string::npos)
            variables[line.substr(0, equals)] = line.substr(equals + 1);
    }
    for (const auto& item : environment)
        variables[item.first] = item.second;
    std::vector<std::string> envStrings;
    for (const auto& item : variables)
        envStrings.push_back(item.first + "=" + item.second);
    std::vector<char*> envp;
    for (std::string& entry : envStrings)
        envp.push_back(entry.data());
    envp.push_back(nullptr);

    std::vector<std::string> argStrings = {command};
    argStrings.insert(argStrings.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (std::string& entry : argStrings)
        argv.push_back(entry.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_adddup2(&actions, err[1], 2);
    // PATH is searched with the child's environment, so a server's own PATH override applies.
    const std::string path = variables.count("PATH") ? variables["PATH"] : std::string();
    std::string resolved = command;
    if (command.find('/') == std::string::npos)
    {
        std::size_t start = 0;
        while (start <= path.size())
        {
            const std::size_t end = path.find(':', start);
            const std::string directory = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
            const std::string candidate = (directory.empty() ? std::string(".") : directory) + "/" + command;
            if (access(candidate.c_str(), X_OK) == 0)
            {
                resolved = candidate;
                break;
            }
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
    // Its own process group, so stopping it reaches grandchildren that share the pipes.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t pid = -1;
    const int code = posix_spawn(&pid, resolved.c_str(), &actions, &attributes, argv.data(), envp.data());
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(in[0]);
    close(out[1]);
    close(err[1]);
    if (code != 0)
    {
        close(in[1]);
        close(out[0]);
        close(err[0]);
        error = "cannot start " + command + ": " + std::strerror(code);
        return nullptr;
    }
    std::unique_ptr<Process> process(new Process());
    process->mNative = std::make_unique<Native>();
    process->mNative->pid = pid;
    process->mNative->input = in[1];
    process->mNative->output = out[0];
    process->mNative->errors = err[0];
    return process;
}

Process::~Process()
{
    if (!mNative)
        return;
    stop(0.0);
    if (mNative->output >= 0)
        close(mNative->output);
    if (mNative->errors >= 0)
        close(mNative->errors);
}

bool Process::write(std::string_view data)
{
    while (!data.empty())
    {
        if (mNative->input < 0)
            return false;
        const ssize_t written = ::write(mNative->input, data.data(), data.size());
        if (written < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

namespace
{
std::size_t readSome(int fd, char* buffer, std::size_t size)
{
    for (;;)
    {
        const ssize_t got = read(fd, buffer, size);
        if (got >= 0)
            return static_cast<std::size_t>(got);
        if (errno != EINTR)
            return 0;
    }
}
} // namespace

std::size_t Process::readOutput(char* buffer, std::size_t size)
{
    return readSome(mNative->output, buffer, size);
}

std::size_t Process::readError(char* buffer, std::size_t size)
{
    return readSome(mNative->errors, buffer, size);
}

void Process::closeInput()
{
    if (mNative->input >= 0)
    {
        close(mNative->input);
        mNative->input = -1;
    }
}

bool Process::exited(int& code)
{
    if (!mNative->reaped)
    {
        // Looked at without reaping: until stop() reaps it, its pid cannot be reused, so the process group can
        // still be signalled safely.
        siginfo_t info = {};
        if (waitid(P_PID, static_cast<id_t>(mNative->pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0 ||
            info.si_pid != mNative->pid)
            return false;
        code = info.si_code == CLD_EXITED ? info.si_status : -1;
        return true;
    }
    code = WIFEXITED(mNative->status) ? WEXITSTATUS(mNative->status) : -1;
    return true;
}

int Process::stop(double seconds)
{
    closeInput();
    int code = -1;
    if (!mNative->reaped)
    {
        auto waitFor = [&](double limit)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(limit);
            while (!exited(code))
            {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return true;
        };
        if (!waitFor(seconds))
        {
            kill(-mNative->pid, SIGTERM);
            waitFor(3.0);
        }
        // The child is alive or a zombie, so the group id is still its own: this reaches whatever it started that
        // may still hold the pipes open.
        kill(-mNative->pid, SIGKILL);
        int status = 0;
        while (waitpid(mNative->pid, &status, 0) < 0 && errno == EINTR)
        {
        }
        mNative->reaped = true;
        mNative->status = status;
    }
    exited(code);
    return code;
}

#endif

} // namespace mcpchat
