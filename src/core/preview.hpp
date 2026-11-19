#pragma once

#include "conversion.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace ffmpegui {

struct PreviewSnapshot {
    std::optional<double> duration;
    std::shared_ptr<const PreviewFrame> frame;
    double frameSeconds{-1.0};
    bool busy{true};
    std::string error;
};

// Performs archive extraction, ffprobe and preview decoding on a worker thread.
// A closed dialog cancels pending work without waiting for FFmpeg.
class AsyncVideoPreview {
public:
    AsyncVideoPreview(FfmpegService service, std::string inputPath,
                      std::string archiveMember = {}, int maximumWidth = 800,
                      int maximumHeight = 374);
    ~AsyncVideoPreview();
    AsyncVideoPreview(const AsyncVideoPreview&) = delete;
    AsyncVideoPreview& operator=(const AsyncVideoPreview&) = delete;

    void seek(double seconds);
    PreviewSnapshot snapshot() const;

private:
    struct State {
        FfmpegService service;
        std::string inputPath;
        std::string archiveMember;
        int maximumWidth{800};
        int maximumHeight{374};
        mutable std::mutex mutex;
        std::condition_variable changed;
        std::atomic_bool cancelled{false};
        std::uint64_t sequence{1};
        double requestedSeconds{0.0};
        PreviewSnapshot current;
        State(FfmpegService ffmpeg, std::string path, std::string member,
              int width, int height)
            : service(std::move(ffmpeg)), inputPath(std::move(path)),
              archiveMember(std::move(member)),
              maximumWidth(width), maximumHeight(height) {}
    };
    static void run(const std::shared_ptr<State>& state);
    std::shared_ptr<State> state_;
};

} // namespace ffmpegui
