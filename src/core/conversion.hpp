#pragma once

#include "ssh.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ffmpegui {

enum class VideoCodec { H264, H265, AV1 };
enum class OutputContainer { MP4, SameAsSource };
enum class EncoderBackend { CPU, NVENC, QSV, AMF };
enum class CompressionLevel { Fastest, VeryFast, Fast, Balanced, Slow, Slower, VerySlow, Extreme };

enum class TaskKind { Video, ZipArchive, PowerPoint };
enum class TaskState { Pending, Running, Completed, Skipped, Failed, Cancelled };

struct ConversionSettings {
    std::string inputPath;
    std::string outputPath;
    // Empty selects local execution. Otherwise this is an SSH Host alias.
    std::string remoteHost;
    // Keeps the original local source identity when the input is staged remotely.
    std::string markerSourcePath;
    OutputContainer outputContainer{OutputContainer::MP4};
    VideoCodec codec{VideoCodec::H264};
    EncoderBackend backend{EncoderBackend::CPU};
    CompressionLevel compression{CompressionLevel::Balanced};
    int crf{23};
    int audioBitrateKbps{192};
    int threadsPerJob{0};
    bool overwrite{false};
    bool skipAlreadyProcessed{true};
};

struct BatchSettings {
    OutputContainer outputContainer{OutputContainer::MP4};
    VideoCodec codec{VideoCodec::H264};
    EncoderBackend backend{EncoderBackend::CPU};
    CompressionLevel compression{CompressionLevel::Balanced};
    int crf{23};
    int audioBitrateKbps{192};
    int maxParallelJobs{0}; // 0 = automatic
    bool recursive{true};
    bool preserveDirectoryStructure{true};
    bool skipAlreadyProcessed{true};
    bool overwrite{false};
    std::string outputDirectory;
    std::string remoteHost;
};

struct EncoderCapabilities {
    std::string ffmpegVersion;
    bool ffprobeAvailable{false};
    bool libx264{false};
    bool libx265{false};
    bool libsvtav1{false};
    bool libaomAv1{false};
    bool libopus{false};
    bool libvorbis{false};
    bool h264Nvenc{false};
    bool hevcNvenc{false};
    bool av1Nvenc{false};
    bool h264Qsv{false};
    bool hevcQsv{false};
    bool av1Qsv{false};
    bool h264Amf{false};
    bool hevcAmf{false};
    bool av1Amf{false};

    bool supports(VideoCodec codec, EncoderBackend backend) const;
    std::string cpuAv1Encoder() const;
};

struct RemoteResources {
    unsigned int logicalCores{0};
    std::uint64_t availableMemoryBytes{0};
    std::string operatingSystem;
};

struct RemoteProbeResult {
    bool success{false};
    EncoderCapabilities capabilities;
    RemoteResources resources;
    std::string error;
};

struct ConversionProgress {
    double fraction{0.0};
    std::string status;
    std::string detail;
    std::optional<std::uint64_t> frame;
    std::optional<double> fps;
    std::optional<double> currentTimeSeconds;
    std::optional<double> durationSeconds;
    std::optional<double> speed;
    std::optional<double> etaSeconds;
    std::optional<std::string> bitrate;
    std::optional<std::uint64_t> totalSizeBytes;
};

struct ConversionResult {
    bool success{false};
    bool cancelled{false};
    bool skipped{false};
    std::string message;
    std::string outputPath;
    std::uint64_t inputSizeBytes{0};
    std::uint64_t outputSizeBytes{0};
};

struct ResourcePlan {
    unsigned int parallelJobs{1};
    unsigned int threadsPerJob{1};
    unsigned int logicalCores{1};
    std::uint64_t availableMemoryBytes{0};
    std::string explanation;
};

using ProgressCallback = std::function<void(const ConversionProgress&)>;

std::string codecName(VideoCodec codec);
std::string outputContainerName(OutputContainer container);
std::string outputVideoExtension(OutputContainer container, const std::string& sourcePath);
std::string backendName(EncoderBackend backend);
std::string compressionName(CompressionLevel level);
std::string taskKindName(TaskKind kind);
std::string taskStateName(TaskState state);
int compressionLevelCount();
int crfMaximum(VideoCodec codec);
int defaultCrf(VideoCodec codec);
std::string settingsFingerprint(const ConversionSettings& settings);
std::string outputSuffix(const ConversionSettings& settings);
ResourcePlan recommendResourcePlan(std::size_t taskCount, EncoderBackend backend,
                                   VideoCodec codec, int userLimit = 0,
                                   unsigned int logicalCoresOverride = 0,
                                   std::uint64_t availableMemoryOverride = 0,
                                   std::string_view resourceLabel = "本机");
bool isSupportedInputFile(const std::string& path);
bool isSupportedArchiveFile(const std::string& path);
bool isVideoFile(const std::string& path);

std::vector<std::string> buildFfmpegCommand(const ConversionSettings& settings,
                                           const EncoderCapabilities& capabilities,
                                           const std::string& ffmpegPath,
                                           const std::string& actualOutputPath);

class FfmpegService {
public:
    explicit FfmpegService(std::string ffmpegPath = "ffmpeg",
                           std::string ffprobePath = "ffprobe");

    EncoderCapabilities probeCapabilities(bool testHardware = true) const;
    RemoteProbeResult probeRemote(const std::string& alias, bool testHardware = true) const;
    SshService& sshService();
    const SshService& sshService() const;
    std::optional<double> probeDuration(const std::string& inputPath) const;
    bool isAlreadyProcessed(const std::string& inputPath,
                            const ConversionSettings& settings) const;
    bool isOutputForSourceAlreadyProcessed(const std::string& outputPath,
                                           const ConversionSettings& settings) const;
    ConversionResult convert(const ConversionSettings& settings,
                             const EncoderCapabilities& capabilities,
                             const ProgressCallback& onProgress,
                             const std::atomic_bool* cancelRequested,
                             const std::atomic_bool* pauseRequested = nullptr) const;

    const std::string& ffmpegPath() const { return ffmpegPath_; }
    const std::string& ffprobePath() const { return ffprobePath_; }

private:
    ConversionResult convertRemote(const ConversionSettings& settings, const EncoderCapabilities& capabilities,
                                   const ProgressCallback& onProgress, const std::atomic_bool* cancelRequested,
                                   const std::atomic_bool* pauseRequested) const;
    bool probeHardwareEncoder(const std::string& encoder) const;
    bool verifyOutput(const std::string& path, VideoCodec codec,
                      const std::string& expectedMarker,
                      const std::string& expectedAudioCodec) const;

    std::string ffmpegPath_;
    std::string ffprobePath_;
    std::shared_ptr<SshService> sshService_;
};

} // namespace ffmpegui
