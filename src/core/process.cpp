#include "process.hpp"

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <codecvt>
#include <locale>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef _WIN32
extern char** environ;
#endif

namespace ffmpegui {
namespace {

void appendOutput(ProcessResult& result, std::string_view chunk, std::string& pending,
                  const ProcessLineCallback& callback) {
    constexpr std::size_t kOutputLimit = 2U * 1024U * 1024U;
    if (result.output.size() < kOutputLimit) {
        const auto remaining = kOutputLimit - result.output.size();
        result.output.append(chunk.substr(0, remaining));
    }

    pending.append(chunk);
    std::size_t newline = 0;
    while ((newline = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, newline);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (callback) {
            callback(line);
        }
        pending.erase(0, newline + 1);
    }
}

void flushPending(std::string& pending, const ProcessLineCallback& callback) {
    if (!pending.empty() && callback) {
        callback(pending);
    }
    pending.clear();
}

#ifdef _WIN32
void resumeThreads(std::vector<HANDLE>& threads) {
    for (HANDLE thread : threads) {
        ResumeThread(thread);
        CloseHandle(thread);
    }
    threads.clear();
}

void suspendProcessThreads(DWORD processId, std::vector<HANDLE>& suspendedThreads) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != processId) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
            if (thread == nullptr) continue;
            if (SuspendThread(thread) != static_cast<DWORD>(-1)) suspendedThreads.push_back(thread);
            else CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

std::wstring quoteWindowsArgument(std::string_view argument) {
    const std::wstring value = wideFromUtf8(argument);
    std::wstring quoted;
    quoted.push_back(L'"');

    std::size_t backslashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}
#endif

} // namespace

std::wstring wideFromUtf8(std::string_view value) {
#ifdef _WIN32
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                              value.data(), static_cast<int>(value.size()),
                                              nullptr, 0);
    if (required <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), required);
    return result;
#else
    try {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        return converter.from_bytes(value.data(), value.data() + value.size());
    } catch (...) {
        return std::wstring(value.begin(), value.end());
    }
#endif
}

std::string utf8FromWide(const wchar_t* value) {
    if (value == nullptr || *value == L'\0') {
        return {};
    }
#ifdef _WIN32
    const int length = static_cast<int>(wcslen(value));
    const int required = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0,
                                              nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, length, result.data(), required,
                        nullptr, nullptr);
    return result;
#else
    try {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        return converter.to_bytes(value);
    } catch (...) {
        return {};
    }
#endif
}

ProcessResult runProcess(const std::vector<std::string>& arguments,
                         const std::atomic_bool* cancelRequested,
                         ProcessLineCallback onLine,
                         const std::atomic_bool* pauseRequested) {
    ProcessResult result;
    if (arguments.empty()) {
        result.output = "No process arguments were supplied.";
        result.exitCode = -1;
        return result;
    }
    if (cancelRequested != nullptr && cancelRequested->load()) {
        result.cancelled = true;
        result.exitCode = 130;
        return result;
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    if (!CreatePipe(&readHandle, &writeHandle, &securityAttributes, 0)) {
        result.exitCode = static_cast<int>(GetLastError());
        result.output = "Unable to create process output pipe.";
        return result;
    }
    SetHandleInformation(readHandle, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdOutput = writeHandle;
    startupInfo.hStdError = writeHandle;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring commandLine;
    for (const auto& argument : arguments) {
        if (!commandLine.empty()) {
            commandLine.push_back(L' ');
        }
        commandLine += quoteWindowsArgument(argument);
    }

    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr,
                                        &startupInfo, &processInfo);
    CloseHandle(writeHandle);
    if (!created) {
        result.exitCode = static_cast<int>(GetLastError());
        result.output = "Unable to start FFmpeg. Check the executable path.";
        CloseHandle(readHandle);
        return result;
    }

    CloseHandle(processInfo.hThread);
    std::string pending;
    bool terminated = false;
    bool paused = false;
    std::vector<HANDLE> suspendedThreads;
    while (true) {
        const bool shouldPause = pauseRequested != nullptr && pauseRequested->load() && !terminated;
        if (shouldPause && !paused) {
            suspendProcessThreads(processInfo.dwProcessId, suspendedThreads);
            paused = !suspendedThreads.empty();
        } else if (!shouldPause && paused) {
            resumeThreads(suspendedThreads);
            paused = false;
        }
        if (cancelRequested != nullptr && cancelRequested->load() && !terminated) {
            if (paused) { resumeThreads(suspendedThreads); paused = false; }
            TerminateProcess(processInfo.hProcess, 130);
            terminated = true;
            result.cancelled = true;
        }

        DWORD available = 0;
        if (PeekNamedPipe(readHandle, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            char buffer[8192];
            DWORD bytesRead = 0;
            const DWORD toRead = std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer)));
            if (ReadFile(readHandle, buffer, toRead, &bytesRead, nullptr) && bytesRead > 0) {
                appendOutput(result, std::string_view(buffer, bytesRead), pending, onLine);
            }
        }

        const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, 40);
        if (waitResult == WAIT_OBJECT_0) {
            DWORD availableAfterExit = 0;
            while (PeekNamedPipe(readHandle, nullptr, 0, nullptr, &availableAfterExit, nullptr) &&
                   availableAfterExit > 0) {
                char buffer[8192];
                DWORD bytesRead = 0;
                const DWORD toRead = std::min<DWORD>(availableAfterExit,
                                                     static_cast<DWORD>(sizeof(buffer)));
                if (!ReadFile(readHandle, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0) {
                    break;
                }
                appendOutput(result, std::string_view(buffer, bytesRead), pending, onLine);
            }
            break;
        }
        if (waitResult == WAIT_FAILED) {
            result.exitCode = static_cast<int>(GetLastError());
            break;
        }
    }

    DWORD exitCode = 1;
    if (GetExitCodeProcess(processInfo.hProcess, &exitCode)) {
        result.exitCode = static_cast<int>(exitCode);
    }
    if (paused) resumeThreads(suspendedThreads);
    flushPending(pending, onLine);
    CloseHandle(processInfo.hProcess);
    CloseHandle(readHandle);
#else
    int pipeDescriptors[2]{};
    if (pipe(pipeDescriptors) != 0) {
        result.exitCode = errno;
        result.output = std::string("Unable to create process output pipe: ") + std::strerror(errno);
        return result;
    }

    std::vector<char*> rawArguments;
    rawArguments.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
        rawArguments.push_back(const_cast<char*>(argument.c_str()));
    }
    rawArguments.push_back(nullptr);

    posix_spawn_file_actions_t fileActions;
    posix_spawnattr_t attributes;
    int spawnError = posix_spawn_file_actions_init(&fileActions);
    const bool fileActionsInitialized = spawnError == 0;
    if (spawnError == 0) {
        spawnError = posix_spawn_file_actions_adddup2(&fileActions, pipeDescriptors[1], STDOUT_FILENO);
    }
    if (spawnError == 0) {
        spawnError = posix_spawn_file_actions_adddup2(&fileActions, pipeDescriptors[1], STDERR_FILENO);
    }
    if (spawnError == 0) {
        spawnError = posix_spawn_file_actions_addclose(&fileActions, pipeDescriptors[0]);
    }
    if (spawnError == 0) {
        spawnError = posix_spawn_file_actions_addclose(&fileActions, pipeDescriptors[1]);
    }
    if (spawnError == 0) {
        spawnError = posix_spawn_file_actions_addopen(&fileActions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }
    bool attributesInitialized = false;
    if (spawnError == 0) {
        spawnError = posix_spawnattr_init(&attributes);
        attributesInitialized = spawnError == 0;
    }
    if (spawnError == 0) {
        spawnError = posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (spawnError == 0) {
        spawnError = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    }

    pid_t child = -1;
    if (spawnError == 0) {
        spawnError = posix_spawnp(&child, rawArguments[0], &fileActions, &attributes,
                                  rawArguments.data(), ::environ);
    }
    if (attributesInitialized) {
        posix_spawnattr_destroy(&attributes);
    }
    if (fileActionsInitialized) {
        posix_spawn_file_actions_destroy(&fileActions);
    }
    if (spawnError != 0) {
        result.exitCode = spawnError;
        result.output = std::string("Unable to start FFmpeg: ") + std::strerror(spawnError);
        close(pipeDescriptors[0]);
        close(pipeDescriptors[1]);
        return result;
    }

    close(pipeDescriptors[1]);
    const int flags = fcntl(pipeDescriptors[0], F_GETFL, 0);
    fcntl(pipeDescriptors[0], F_SETFL, flags | O_NONBLOCK);

    std::string pending;
    bool terminated = false;
    bool paused = false;
    auto terminateTime = std::chrono::steady_clock::time_point{};
    bool childExited = false;
    int childStatus = 0;

    while (true) {
        const bool shouldPause = pauseRequested != nullptr && pauseRequested->load() && !terminated;
        if (shouldPause && !paused && kill(-child, SIGSTOP) == 0) paused = true;
        else if (!shouldPause && paused) { kill(-child, SIGCONT); paused = false; }
        if (cancelRequested != nullptr && cancelRequested->load() && !terminated) {
            if (paused) { kill(-child, SIGCONT); paused = false; }
            kill(-child, SIGTERM);
            terminated = true;
            result.cancelled = true;
            terminateTime = std::chrono::steady_clock::now();
        }
        if (terminated && !childExited &&
            std::chrono::steady_clock::now() - terminateTime > std::chrono::seconds(2)) {
            kill(-child, SIGKILL);
        }

        pollfd descriptor{pipeDescriptors[0], POLLIN | POLLHUP, 0};
        const int pollResult = poll(&descriptor, 1, 60);
        if (pollResult > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0) {
            char buffer[8192];
            while (true) {
                const ssize_t bytesRead = read(pipeDescriptors[0], buffer, sizeof(buffer));
                if (bytesRead > 0) {
                    appendOutput(result, std::string_view(buffer, static_cast<std::size_t>(bytesRead)),
                                 pending, onLine);
                    continue;
                }
                if (bytesRead < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
        }

        if (!childExited) {
            const pid_t waitResult = waitpid(child, &childStatus, WNOHANG);
            if (waitResult == child) {
                childExited = true;
            }
        }

        if (childExited && (pollResult == 0 || (descriptor.revents & POLLHUP) != 0)) {
            char buffer[8192];
            const ssize_t bytesRead = read(pipeDescriptors[0], buffer, sizeof(buffer));
            if (bytesRead > 0) {
                appendOutput(result, std::string_view(buffer, static_cast<std::size_t>(bytesRead)),
                             pending, onLine);
                continue;
            }
            if (bytesRead == 0 || (bytesRead < 0 && errno != EINTR && errno != EAGAIN)) {
                break;
            }
            if (pollResult == 0 && bytesRead < 0 && errno == EAGAIN) {
                break;
            }
        }
    }

    if (paused) kill(-child, SIGCONT);
    if (!childExited) {
        waitpid(child, &childStatus, 0);
    }
    if (WIFEXITED(childStatus)) {
        result.exitCode = WEXITSTATUS(childStatus);
    } else if (WIFSIGNALED(childStatus)) {
        result.exitCode = 128 + WTERMSIG(childStatus);
    }
    flushPending(pending, onLine);
    close(pipeDescriptors[0]);
#endif

    return result;
}

} // namespace ffmpegui
