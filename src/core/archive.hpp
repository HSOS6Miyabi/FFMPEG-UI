#pragma once

#include "conversion.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace ffmpegui {

struct ArchiveItemProgress {
    std::string name;
    TaskState state{TaskState::Pending};
    ConversionProgress progress;
    std::string detail;
    std::uint64_t inputSizeBytes{0};
    std::uint64_t outputSizeBytes{0};
};

struct ArchiveProgressSnapshot {
    std::vector<ArchiveItemProgress> items;
    double fraction{0.0};
    std::string status;
    std::string detail;
};

struct ArchiveResult {
    bool success{false};
    bool cancelled{false};
    bool skipped{false};
    std::string message;
    std::string outputPath;
    std::size_t processedVideos{0};
    std::size_t skippedVideos{0};
    std::size_t totalVideos{0};
    std::uint64_t inputSizeBytes{0};
    std::uint64_t outputSizeBytes{0};
};

using ArchiveProgressCallback = std::function<void(const ArchiveProgressSnapshot&)>;
using ArchiveVideoCallback = std::function<void(const std::string&)>;

// Inspect archive headers without extracting media; safe for use on a background thread.
bool inspectArchiveVideos(const std::string& inputPath,
                          const ArchiveVideoCallback& onVideo,
                          std::string& error,
                          const std::atomic_bool* cancelRequested = nullptr);

class ArchiveService {
public:
    explicit ArchiveService(const FfmpegService& ffmpeg);

    ArchiveResult process(const std::string& inputPath,
                          const std::string& outputPath,
                          bool isPowerPoint,
                          const ConversionSettings& settings,
                          const EncoderCapabilities& capabilities,
                          unsigned int parallelJobs,
                          unsigned int threadsPerJob,
                          const ArchiveProgressCallback& onProgress,
                          const std::atomic_bool* cancelRequested,
                          const std::atomic_bool* pauseRequested = nullptr,
                          const VideoTrimRanges& perVideoTrims = {}) const;

private:
    const FfmpegService& ffmpeg_;
};

} // namespace ffmpegui
