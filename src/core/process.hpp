#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ffmpegui {

struct ProcessResult {
    int exitCode{-1};
    bool cancelled{false};
    std::string output;
};

using ProcessLineCallback = std::function<void(std::string_view)>;

ProcessResult runProcess(const std::vector<std::string>& arguments,
                         const std::atomic_bool* cancelRequested = nullptr,
                         ProcessLineCallback onLine = {},
                         const std::atomic_bool* pauseRequested = nullptr);

std::string utf8FromWide(const wchar_t* value);
std::wstring wideFromUtf8(std::string_view value);

} // namespace ffmpegui
