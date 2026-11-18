#include "conversion.hpp"

#include "process.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/sysinfo.h>
#endif

namespace ffmpegui {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::filesystem::path pathFromUtf8(std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character : value) encoded.push_back(static_cast<char8_t>(character));
    return std::filesystem::path(encoded);
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string encoderFor(VideoCodec codec, EncoderBackend backend,
                       const EncoderCapabilities& capabilities) {
    switch (backend) {
    case EncoderBackend::CPU:
        switch (codec) {
        case VideoCodec::H264: return "libx264";
        case VideoCodec::H265: return "libx265";
        case VideoCodec::AV1: return capabilities.cpuAv1Encoder();
        }
        break;
    case EncoderBackend::NVENC:
        switch (codec) {
        case VideoCodec::H264: return "h264_nvenc";
        case VideoCodec::H265: return "hevc_nvenc";
        case VideoCodec::AV1: return "av1_nvenc";
        }
        break;
    case EncoderBackend::QSV:
        switch (codec) {
        case VideoCodec::H264: return "h264_qsv";
        case VideoCodec::H265: return "hevc_qsv";
        case VideoCodec::AV1: return "av1_qsv";
        }
        break;
    case EncoderBackend::AMF:
        switch (codec) {
        case VideoCodec::H264: return "h264_amf";
        case VideoCodec::H265: return "hevc_amf";
        case VideoCodec::AV1: return "av1_amf";
        }
        break;
    }
    return {};
}

std::string cpuPreset(VideoCodec codec, CompressionLevel level, bool useSvtAv1) {
    static constexpr const char* presets[] = {
        "ultrafast", "veryfast", "fast", "medium", "slow", "slower", "veryslow", "placebo"};
    static constexpr int svtPresets[] = {13, 11, 10, 8, 6, 4, 2, 0};
    static constexpr int aomCpuUsed[] = {8, 7, 6, 4, 3, 2, 1, 0};
    const auto index = static_cast<std::size_t>(level);
    if (codec == VideoCodec::AV1) {
        return std::to_string(useSvtAv1 ? svtPresets[index] : aomCpuUsed[index]);
    }
    return presets[index];
}

std::string nvencPreset(CompressionLevel level) {
    static constexpr const char* presets[] = {"p1", "p2", "p3", "p4", "p5", "p6", "p7", "p7"};
    return presets[static_cast<std::size_t>(level)];
}

std::string qsvPreset(CompressionLevel level) {
    static constexpr const char* presets[] = {
        "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", "veryslow"};
    return presets[static_cast<std::size_t>(level)];
}

std::string amfQuality(CompressionLevel level) {
    if (level <= CompressionLevel::VeryFast) return "speed";
    if (level <= CompressionLevel::Balanced) return "balanced";
    return "quality";
}

std::optional<double> parseNumber(std::string_view value) {
    try {
        std::size_t parsed = 0;
        const double number = std::stod(std::string(value), &parsed);
        if (parsed == 0 || !std::isfinite(number)) return std::nullopt;
        return number;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::uint64_t> parseUnsigned(std::string_view value) {
    try {
        std::size_t parsed = 0;
        const auto number = std::stoull(std::string(value), &parsed);
        if (parsed == 0) return std::nullopt;
        return static_cast<std::uint64_t>(number);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> parseTimestamp(std::string_view value) {
    const auto firstColon = value.find(':');
    const auto secondColon = firstColon == std::string_view::npos
                                 ? std::string_view::npos
                                 : value.find(':', firstColon + 1);
    if (firstColon == std::string_view::npos || secondColon == std::string_view::npos) return std::nullopt;
    try {
        const double hours = std::stod(std::string(value.substr(0, firstColon)));
        const double minutes = std::stod(std::string(value.substr(firstColon + 1,
                                                                  secondColon - firstColon - 1)));
        const double seconds = std::stod(std::string(value.substr(secondColon + 1)));
        return hours * 3600.0 + minutes * 60.0 + seconds;
    } catch (...) {
        return std::nullopt;
    }
}

std::string formatProgressDetail(const ConversionProgress& progress) {
    std::ostringstream detail;
    if (progress.currentTimeSeconds && progress.durationSeconds && *progress.durationSeconds > 0.0) {
        detail << "已处理 " << static_cast<int>(std::clamp(
            *progress.currentTimeSeconds / *progress.durationSeconds * 100.0, 0.0, 99.0)) << "%";
    } else {
        detail << "正在编码";
    }
    if (progress.speed && *progress.speed > 0.0) {
        detail << " · " << std::fixed << std::setprecision(2) << *progress.speed << "x";
    }
    if (progress.etaSeconds && *progress.etaSeconds >= 0.0) {
        const auto seconds = static_cast<std::uint64_t>(*progress.etaSeconds);
        detail << " · 剩余约 " << seconds / 60 << " 分 " << seconds % 60 << " 秒";
    }
    return detail.str();
}

std::string processErrorSummary(const std::string& output) {
    std::istringstream stream(output);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(std::move(line));
        if (!line.empty()) lines.push_back(std::move(line));
    }
    if (lines.empty()) return "FFmpeg 无法完成转换，请检查输入文件、编码器和输出目录。";
    const std::size_t start = lines.size() > 8 ? lines.size() - 8 : 0;
    std::ostringstream summary;
    for (std::size_t index = start; index < lines.size(); ++index) {
        if (index != start) summary << '\n';
        summary << lines[index];
    }
    return summary.str();
}

std::filesystem::path makeTemporaryPath(const std::filesystem::path& output) {
    static std::atomic_uint64_t sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto unique = static_cast<unsigned long long>(timestamp) + sequence.fetch_add(1);
    const std::string extension = pathToUtf8(output.extension());
    const std::string name = pathToUtf8(output.stem()) + ".ffmpegui-" +
                             std::to_string(unique) + ".tmp" + extension;
    return output.parent_path() / pathFromUtf8(name);
}

bool replaceFile(const std::filesystem::path& source, const std::filesystem::path& destination,
                 bool overwrite, std::string& error) {
#ifdef _WIN32
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(source.c_str(), destination.c_str(), flags)) {
        error = "无法将临时文件保存到目标位置。Windows 错误代码：" + std::to_string(GetLastError());
        return false;
    }
    return true;
#else
    std::error_code code;
    if (!overwrite && std::filesystem::exists(destination, code)) {
        error = "输出文件已经存在。";
        return false;
    }
    std::filesystem::rename(source, destination, code);
    if (code) {
        error = "无法将临时文件保存到目标位置：" + code.message();
        return false;
    }
    return true;
#endif
}

std::string expectedStreamCodec(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264: return "h264";
    case VideoCodec::H265: return "hevc";
    case VideoCodec::AV1: return "av1";
    }
    return {};
}

std::string expectedAudioCodecName(const std::string& encoder) {
    if (encoder == "libopus") return "opus";
    if (encoder == "libvorbis") return "vorbis";
    return encoder;
}

std::string expectedAudioCodecForSettings(AudioMode mode, const std::string& encoder) {
    switch (mode) {
    case AudioMode::Encode: return expectedAudioCodecName(encoder);
    case AudioMode::Copy: return "copy";
    case AudioMode::Remove: return "none";
    }
    return "none";
}

std::string outputScaleFilter(OutputResolution resolution) {
    int maximumWidth = 0;
    int maximumHeight = 0;
    switch (resolution) {
    case OutputResolution::Original: return {};
    case OutputResolution::UHD4K: maximumWidth = 3840; maximumHeight = 2160; break;
    case OutputResolution::QHD1440: maximumWidth = 2560; maximumHeight = 1440; break;
    case OutputResolution::FullHD1080: maximumWidth = 1920; maximumHeight = 1080; break;
    case OutputResolution::HD720: maximumWidth = 1280; maximumHeight = 720; break;
    case OutputResolution::SD480: maximumWidth = 854; maximumHeight = 480; break;
    }
    return "scale=w='min(iw," + std::to_string(maximumWidth) +
        ")':h='min(ih," + std::to_string(maximumHeight) +
        ")':force_original_aspect_ratio=decrease:force_divisible_by=2";
}

std::pair<int, int> outputResolutionBounds(OutputResolution resolution) {
    switch (resolution) {
    case OutputResolution::Original: return {0, 0};
    case OutputResolution::UHD4K: return {3840, 2160};
    case OutputResolution::QHD1440: return {2560, 1440};
    case OutputResolution::FullHD1080: return {1920, 1080};
    case OutputResolution::HD720: return {1280, 720};
    case OutputResolution::SD480: return {854, 480};
    }
    return {0, 0};
}

std::optional<int> integerFieldValue(const std::string& text, std::string_view field) {
    const std::size_t position = text.find(field);
    if (position == std::string::npos) return std::nullopt;
    const std::size_t start = position + field.size();
    const std::size_t end = text.find_first_not_of("0123456789", start);
    try {
        return std::stoi(text.substr(start, end == std::string::npos
            ? std::string::npos : end - start));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool outputResolutionMatches(const std::string& format, OutputResolution resolution) {
    const auto [maximumWidth, maximumHeight] = outputResolutionBounds(resolution);
    if (maximumWidth == 0 || maximumHeight == 0) return true;
    const auto width = integerFieldValue(format, "width=");
    const auto height = integerFieldValue(format, "height=");
    return width && height && *width > 0 && *height > 0 &&
        *width <= maximumWidth && *height <= maximumHeight;
}

std::string audioEncoderForExtension(const std::string& extension,
                                    const EncoderCapabilities& capabilities) {
    if (extension == ".webm" || extension == ".ogv") {
        if (capabilities.libopus) return "libopus";
        if (capabilities.libvorbis) return "libvorbis";
        return {};
    }
    if (extension == ".wmv") return "wmav2";
    if (extension == ".mpg" || extension == ".mpeg" || extension == ".vob") return "mp2";
    return "aac";
}

bool containerSupportsVideoCodec(const std::string& extension, VideoCodec codec) {
    if (extension == ".ogv") return false;
    if (extension == ".webm") return codec == VideoCodec::AV1;
    if (extension == ".mov") return codec != VideoCodec::AV1;
    if (extension == ".wmv") return codec != VideoCodec::H265;
    if (extension == ".3gp") return codec == VideoCodec::H264;
    return true;
}

std::string sourceFingerprint(const std::string& inputPath) {
    namespace fs = std::filesystem;
    const fs::path path = pathFromUtf8(inputPath);
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(path, error);
    const std::string canonicalName = pathToUtf8(error ? path.lexically_normal() : canonical);
    error.clear();
    const std::uint64_t size = fs::is_regular_file(path, error) ? fs::file_size(path, error) : 0;
    error.clear();
    const auto modified = fs::last_write_time(path, error);
    const auto modifiedTicks = error ? 0LL : static_cast<long long>(modified.time_since_epoch().count());
    const std::string identity = canonicalName + "|" + std::to_string(size) + "|" + std::to_string(modifiedTicks);
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : identity) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream result;
    result << std::hex << std::setw(16) << std::setfill('0') << hash;
    return result.str();
}

std::string processingMarker(const ConversionSettings& settings) {
    const std::string& source = settings.markerSourcePath.empty() ? settings.inputPath : settings.markerSourcePath;
    return "VideoCompressionWorkbench:v3:" + settingsFingerprint(settings) + ":" + sourceFingerprint(source);
}

std::uint64_t availableMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX state{};
    state.dwLength = sizeof(state);
    if (GlobalMemoryStatusEx(&state)) return state.ullAvailPhys;
    return 0;
#else
    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    std::uint64_t value = 0;
    std::string unit;
    while (meminfo >> key >> value >> unit) {
        if (key == "MemAvailable:") return value * 1024ULL;
    }
    struct sysinfo info{};
    if (sysinfo(&info) == 0) {
        return static_cast<std::uint64_t>(info.freeram + info.bufferram) * info.mem_unit;
    }
    return 0;
#endif
}

std::string codecShortName(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264: return "H264";
    case VideoCodec::H265: return "H265";
    case VideoCodec::AV1: return "AV1";
    }
    return "VIDEO";
}

std::string backendShortName(EncoderBackend backend) {
    switch (backend) {
    case EncoderBackend::CPU: return "CPU";
    case EncoderBackend::NVENC: return "NVENC";
    case EncoderBackend::QSV: return "QSV";
    case EncoderBackend::AMF: return "AMF";
    }
    return "AUTO";
}


bool outputDescriptionMatches(const std::string& format,
                              const std::string& path,
                              VideoCodec codec,
                              const std::string& marker,
                              const std::string& expectedAudioCodec,
                              OutputResolution resolution) {
    const std::string extension = lower(pathToUtf8(pathFromUtf8(path).extension()));
    const std::string expectedVideoCodec = "codec_name=" + expectedStreamCodec(codec);
    const bool hasVideo = format.find(expectedVideoCodec) != std::string::npos;

    bool hasExpectedContainer = false;
    if (extension == ".mp4" || extension == ".m4v" ||
        extension == ".mov" || extension == ".3gp") {
        hasExpectedContainer =
            format.find("mov,") != std::string::npos ||
            format.find("mp4") != std::string::npos;
    } else if (extension == ".mkv") {
        hasExpectedContainer = format.find("matroska") != std::string::npos;
    } else if (extension == ".webm") {
        hasExpectedContainer = format.find("webm") != std::string::npos;
    } else if (extension == ".avi") {
        hasExpectedContainer = format.find("avi") != std::string::npos;
    } else if (extension == ".wmv") {
        hasExpectedContainer = format.find("asf") != std::string::npos;
    } else if (extension == ".flv") {
        hasExpectedContainer = format.find("flv") != std::string::npos;
    } else if (extension == ".ts" || extension == ".m2ts" || extension == ".mts") {
        hasExpectedContainer = format.find("mpegts") != std::string::npos;
    } else if (extension == ".mpg" || extension == ".mpeg" || extension == ".vob") {
        hasExpectedContainer = format.find("mpeg") != std::string::npos;
    } else if (extension == ".ogv") {
        hasExpectedContainer = format.find("ogg") != std::string::npos;
    }

    const bool hasAudio = format.find("codec_type=audio") != std::string::npos;
    const bool audioMatches = expectedAudioCodec == "none"
        ? !hasAudio
        : expectedAudioCodec == "copy" ||
            !hasAudio ||
            format.find("codec_name=" + expectedAudioCodec) != std::string::npos;
    const bool markerMatches = format.find(marker) != std::string::npos;
    const bool resolutionMatches = outputResolutionMatches(format, resolution);

    return hasVideo && hasExpectedContainer && audioMatches &&
        markerMatches && resolutionMatches;
}

std::string joinRemoteArguments(const std::vector<std::string>& arguments) {
    std::string result;
    for (const auto& argument : arguments) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += SshService::shellQuote(argument);
    }
    return result;
}

std::uint64_t parseRemoteSize(const std::string& value) {
    try {
        return static_cast<std::uint64_t>(std::stoull(trim(value)));
    } catch (const std::exception&) {
        return 0;
    }
}

} // namespace

bool EncoderCapabilities::supports(VideoCodec codec, EncoderBackend backend) const {
    switch (backend) {
    case EncoderBackend::CPU:
        switch (codec) {
        case VideoCodec::H264: return libx264;
        case VideoCodec::H265: return libx265;
        case VideoCodec::AV1: return libsvtav1 || libaomAv1;
        }
        break;
    case EncoderBackend::NVENC:
        switch (codec) {
        case VideoCodec::H264: return h264Nvenc;
        case VideoCodec::H265: return hevcNvenc;
        case VideoCodec::AV1: return av1Nvenc;
        }
        break;
    case EncoderBackend::QSV:
        switch (codec) {
        case VideoCodec::H264: return h264Qsv;
        case VideoCodec::H265: return hevcQsv;
        case VideoCodec::AV1: return av1Qsv;
        }
        break;
    case EncoderBackend::AMF:
        switch (codec) {
        case VideoCodec::H264: return h264Amf;
        case VideoCodec::H265: return hevcAmf;
        case VideoCodec::AV1: return av1Amf;
        }
        break;
    }
    return false;
}

std::string EncoderCapabilities::cpuAv1Encoder() const {
    if (libsvtav1) return "libsvtav1";
    if (libaomAv1) return "libaom-av1";
    return {};
}

std::string outputContainerName(OutputContainer container) {
    switch (container) {
    case OutputContainer::MP4: return "MP4";
    case OutputContainer::SameAsSource: return "与源文件相同";
    }
    return "MP4";
}

std::string outputVideoExtension(OutputContainer container, const std::string& sourcePath) {
    if (container == OutputContainer::MP4) return ".mp4";
    const auto extension = pathFromUtf8(sourcePath).extension();
    std::string value = pathToUtf8(extension);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string codecName(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264: return "H.264";
    case VideoCodec::H265: return "H.265";
    case VideoCodec::AV1: return "AV1";
    }
    return "未知编码";
}

std::string backendName(EncoderBackend backend) {
    switch (backend) {
    case EncoderBackend::CPU: return "CPU";
    case EncoderBackend::NVENC: return "NVIDIA NVENC";
    case EncoderBackend::QSV: return "Intel Quick Sync";
    case EncoderBackend::AMF: return "AMD AMF";
    }
    return "未知编码器";
}

std::string compressionName(CompressionLevel level) {
    switch (level) {
    case CompressionLevel::Fastest: return "极速";
    case CompressionLevel::VeryFast: return "非常快";
    case CompressionLevel::Fast: return "快速";
    case CompressionLevel::Balanced: return "均衡";
    case CompressionLevel::Slow: return "慢速";
    case CompressionLevel::Slower: return "较慢";
    case CompressionLevel::VerySlow: return "很慢";
    case CompressionLevel::Extreme: return "极致压缩";
    }
    return "均衡";
}

std::string audioModeName(AudioMode mode) {
    switch (mode) {
    case AudioMode::Encode: return "自动编码";
    case AudioMode::Copy: return "保留原音频";
    case AudioMode::Remove: return "移除音频";
    }
    return "自动编码";
}

std::string outputResolutionName(OutputResolution resolution) {
    switch (resolution) {
    case OutputResolution::Original: return "原始尺寸";
    case OutputResolution::UHD4K: return "最高 2160p";
    case OutputResolution::QHD1440: return "最高 1440p";
    case OutputResolution::FullHD1080: return "最高 1080p";
    case OutputResolution::HD720: return "最高 720p";
    case OutputResolution::SD480: return "最高 480p";
    }
    return "原始尺寸";
}

std::string taskKindName(TaskKind kind) {
    switch (kind) {
    case TaskKind::Video: return "视频";
    case TaskKind::ZipArchive: return "ZIP 压缩包";
    case TaskKind::PowerPoint: return "PowerPoint";
    }
    return "文件";
}

std::string taskStateName(TaskState state) {
    switch (state) {
    case TaskState::Pending: return "等待中";
    case TaskState::Running: return "处理中";
    case TaskState::Completed: return "已完成";
    case TaskState::Skipped: return "已跳过";
    case TaskState::Failed: return "失败";
    case TaskState::Cancelled: return "已取消";
    }
    return "未知";
}

int compressionLevelCount() { return 8; }
int crfMaximum(VideoCodec codec) { return codec == VideoCodec::AV1 ? 63 : 51; }

int defaultCrf(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264: return 23;
    case VideoCodec::H265: return 28;
    case VideoCodec::AV1: return 30;
    }
    return 23;
}

std::string settingsFingerprint(const ConversionSettings& settings) {
    std::ostringstream value;
    const std::string& source = settings.markerSourcePath.empty() ? settings.inputPath : settings.markerSourcePath;
    value << "codec=" << codecShortName(settings.codec)
          << ";backend=" << backendShortName(settings.backend)
          << ";quality=" << settings.crf
          << ";compression=" << static_cast<int>(settings.compression)
          << ";audio_mode=" << static_cast<int>(settings.audioMode);
    if (settings.audioMode == AudioMode::Encode) {
        value << ";audio=" << settings.audioBitrateKbps;
    }
    value << ";resolution=" << static_cast<int>(settings.outputResolution)
          << ";container=" << (settings.outputContainer == OutputContainer::MP4
                ? "mp4" : "source:" + outputVideoExtension(settings.outputContainer, source));
    return value.str();
}

std::string outputSuffix(const ConversionSettings& settings) {
    const std::string qualityType = settings.backend == EncoderBackend::CPU ? "CR" : "CQ";
    const std::string container = settings.outputContainer == OutputContainer::SameAsSource ? "_SRC" : "";
    const std::string audio = settings.audioMode == AudioMode::Encode
        ? "_A" + std::to_string(settings.audioBitrateKbps)
        : settings.audioMode == AudioMode::Copy ? "_ACOPY" : "_ANOAUDIO";
    std::string resolution;
    switch (settings.outputResolution) {
    case OutputResolution::Original: break;
    case OutputResolution::UHD4K: resolution = "_R4K"; break;
    case OutputResolution::QHD1440: resolution = "_R1440"; break;
    case OutputResolution::FullHD1080: resolution = "_R1080"; break;
    case OutputResolution::HD720: resolution = "_R720"; break;
    case OutputResolution::SD480: resolution = "_R480"; break;
    }
    return "_" + codecShortName(settings.codec) + "_" + backendShortName(settings.backend) +
           "_" + qualityType + std::to_string(settings.crf) + "_" +
           compressionName(settings.compression) + audio + resolution + container;
}

namespace {
std::string legacySettingsFingerprint(const ConversionSettings& settings) {
    std::ostringstream value;
    const std::string& source = settings.markerSourcePath.empty() ? settings.inputPath : settings.markerSourcePath;
    value << "codec=" << codecShortName(settings.codec)
          << ";backend=" << backendShortName(settings.backend)
          << ";quality=" << settings.crf
          << ";compression=" << static_cast<int>(settings.compression)
          << ";audio=" << settings.audioBitrateKbps
          << ";container=" << (settings.outputContainer == OutputContainer::MP4
                ? "mp4" : "source:" + outputVideoExtension(settings.outputContainer, source));
    return value.str();
}

bool supportsLegacyMarker(const ConversionSettings& settings) {
    return settings.audioMode == AudioMode::Encode &&
        settings.outputResolution == OutputResolution::Original;
}
} // namespace

ResourcePlan recommendResourcePlan(std::size_t taskCount, EncoderBackend backend,
                                   VideoCodec codec, int userLimit,
                                   unsigned int logicalCoresOverride,
                                   std::uint64_t availableMemoryOverride,
                                   std::string_view resourceLabel) {
    ResourcePlan plan;
    const unsigned int detectedCores =
        logicalCoresOverride == 0
            ? std::thread::hardware_concurrency()
            : logicalCoresOverride;
    plan.logicalCores = std::max(1U, detectedCores);
    plan.availableMemoryBytes = availableMemoryOverride == 0 ? availableMemory() : availableMemoryOverride;
    if (taskCount == 0) {
        plan.parallelJobs = 0;
        plan.threadsPerJob = plan.logicalCores;
        plan.explanation = "尚无待处理任务";
        return plan;
    }

    const bool hardware = backend != EncoderBackend::CPU;
    const std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
    const std::uint64_t memoryPerJob = hardware ? 1536ULL * 1024ULL * 1024ULL
        : codec == VideoCodec::AV1 ? 3ULL * gib : 2ULL * gib;
    const unsigned int cpuBudget = plan.logicalCores;
    unsigned int byMemory = plan.availableMemoryBytes == 0 ? cpuBudget
        : static_cast<unsigned int>(std::max<std::uint64_t>(1, plan.availableMemoryBytes / memoryPerJob));
    unsigned int desired = std::min<unsigned int>(static_cast<unsigned int>(taskCount),
                                                   std::min(cpuBudget, byMemory));
    if (hardware) desired = std::min(desired, 2U);
    if (userLimit > 0) desired = std::min(desired, static_cast<unsigned int>(userLimit));
    plan.parallelJobs = std::max(1U, desired);
    plan.threadsPerJob = std::max(1U, (plan.logicalCores + plan.parallelJobs - 1U) / plan.parallelJobs);
    if (hardware) plan.threadsPerJob = std::max(1U, (plan.threadsPerJob + 1U) / 2U);

    std::ostringstream explanation;
    explanation << resourceLabel << "自动调度：" << plan.parallelJobs << " 个并行任务，每个任务分配 "
                << plan.threadsPerJob << " 个编码线程；按可用内存限制任务数量，尽量使用全部 CPU 核心。检测到 " << plan.logicalCores
                << " 个逻辑 CPU 核心";
    if (plan.availableMemoryBytes > 0) {
        explanation << "，可用内存约 " << std::fixed << std::setprecision(1)
                    << static_cast<double>(plan.availableMemoryBytes) / static_cast<double>(gib) << " GiB";
    }
    if (hardware) explanation << "，硬件编码并发数限制为 2";
    plan.explanation = explanation.str();
    return plan;
}

bool isVideoFile(const std::string& path) {
    static const std::vector<std::string> extensions{
        ".mp4", ".m4v", ".mov", ".mkv", ".avi", ".webm", ".wmv", ".flv",
        ".mpg", ".mpeg", ".3gp", ".ts", ".m2ts", ".mts", ".vob", ".ogv"};
    const std::string extension = lower(pathToUtf8(pathFromUtf8(path).extension()));
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

bool isSupportedArchiveFile(const std::string& path) {
    const std::string extension = lower(pathToUtf8(pathFromUtf8(path).extension()));
    return extension == ".zip" || extension == ".pptx";
}

bool isSupportedInputFile(const std::string& path) {
    return isVideoFile(path) || isSupportedArchiveFile(path);
}

std::vector<std::string> buildFfmpegCommand(const ConversionSettings& settings,
                                            const EncoderCapabilities& capabilities,
                                            const std::string& ffmpegPath,
                                            const std::string& actualOutputPath) {
    if (!capabilities.supports(settings.codec, settings.backend))
        throw std::invalid_argument("当前 FFmpeg 环境不支持所选编码器。");
    if (settings.crf < 0 || settings.crf > crfMaximum(settings.codec))
        throw std::invalid_argument("CRF 数值超出当前编码格式的范围。");
    if (settings.audioMode == AudioMode::Encode &&
        (settings.audioBitrateKbps < 64 || settings.audioBitrateKbps > 320)) {
        throw std::invalid_argument("音频码率必须介于 64 到 320 kbps 之间。");
    }

    const std::string encoder = encoderFor(settings.codec, settings.backend, capabilities);
    const std::string outputExtension = lower(pathToUtf8(pathFromUtf8(actualOutputPath).extension()));
    if (!containerSupportsVideoCodec(outputExtension, settings.codec))
        throw std::invalid_argument("所选视频编码格式与目标容器不兼容。请更换编码格式或选择 MP4 输出。");
    const std::string audioEncoder = settings.audioMode == AudioMode::Encode
        ? audioEncoderForExtension(outputExtension, capabilities) : std::string{};
    if (settings.audioMode == AudioMode::Encode && audioEncoder.empty())
        throw std::invalid_argument("当前 FFmpeg 未提供该容器所需的音频编码器。");
    const bool useSvtAv1 = encoder == "libsvtav1";
    std::vector<std::string> command{
        ffmpegPath, "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
        "-i", settings.inputPath,
        "-map", "0:v:0"
    };
    if (settings.audioMode != AudioMode::Remove) {
        command.insert(command.end(), {"-map", "0:a?"});
    }
    command.insert(command.end(), {"-map_metadata", "0", "-map_chapters", "0", "-c:v", encoder});

    switch (settings.backend) {
    case EncoderBackend::CPU:
        if (settings.codec == VideoCodec::AV1 && !useSvtAv1) {
            command.insert(command.end(), {"-crf", std::to_string(settings.crf), "-b:v", "0",
                "-cpu-used", cpuPreset(settings.codec, settings.compression, false)});
        } else {
            command.insert(command.end(), {"-preset", cpuPreset(settings.codec, settings.compression, useSvtAv1),
                "-crf", std::to_string(settings.crf)});
        }
        break;
    case EncoderBackend::NVENC:
        command.insert(command.end(), {"-preset", nvencPreset(settings.compression), "-tune", "hq",
            "-rc", "vbr", "-cq", std::to_string(settings.crf), "-b:v", "0"});
        if (settings.compression == CompressionLevel::Extreme)
            command.insert(command.end(), {"-multipass", "fullres"});
        break;
    case EncoderBackend::QSV:
        command.insert(command.end(), {"-preset", qsvPreset(settings.compression),
            "-global_quality", std::to_string(settings.crf)});
        if (settings.compression >= CompressionLevel::Slow)
            command.insert(command.end(), {"-look_ahead", "1"});
        break;
    case EncoderBackend::AMF:
        command.insert(command.end(), {"-quality", amfQuality(settings.compression),
            "-rc", "cqp", "-qp_i", std::to_string(settings.crf),
            "-qp_p", std::to_string(settings.crf)});
        break;
    }

    if (settings.threadsPerJob > 0)
        command.insert(command.end(), {"-threads:v", std::to_string(settings.threadsPerJob)});

    const std::string marker = processingMarker(settings);
    const std::string scaleFilter = outputScaleFilter(settings.outputResolution);
    if (!scaleFilter.empty()) {
        command.insert(command.end(), {"-vf", scaleFilter});
    }
    command.insert(command.end(), {"-pix_fmt", "yuv420p"});
    if (settings.audioMode == AudioMode::Encode) {
        command.insert(command.end(), {
            "-c:a", audioEncoder, "-b:a", std::to_string(settings.audioBitrateKbps) + "k"});
    } else if (settings.audioMode == AudioMode::Copy) {
        command.insert(command.end(), {"-c:a", "copy"});
    }
    command.insert(command.end(), {"-metadata", "comment=" + marker});
    const bool movFamily = outputExtension == ".mp4" || outputExtension == ".m4v" ||
        outputExtension == ".mov" || outputExtension == ".3gp";
    if (movFamily && settings.codec == VideoCodec::H265) command.insert(command.end(), {"-tag:v", "hvc1"});
    if (movFamily && settings.codec == VideoCodec::AV1) command.insert(command.end(), {"-tag:v", "av01"});
    if (movFamily) command.insert(command.end(), {"-movflags", "+faststart"});
    command.insert(command.end(), {"-progress", "pipe:1", "-stats_period", "0.25", "-nostats", actualOutputPath});
    return command;
}

FfmpegService::FfmpegService(std::string ffmpegPath, std::string ffprobePath)
    : ffmpegPath_(std::move(ffmpegPath)), ffprobePath_(std::move(ffprobePath)),
      sshService_(std::make_shared<SshService>()) {}

SshService& FfmpegService::sshService() { return *sshService_; }
const SshService& FfmpegService::sshService() const { return *sshService_; }

RemoteProbeResult FfmpegService::probeRemote(const std::string& alias,
                                                       bool testHardware) const {
    RemoteProbeResult result;

    // Keep the remote command POSIX-compatible; the remote processing backend currently targets Linux.
    const std::string command =
        "command -v ffmpeg >/dev/null 2>&1 && "
        "command -v ffprobe >/dev/null 2>&1 && "
        "ffmpeg -version && "
        "ffmpeg -hide_banner -encoders && "
        "ffprobe -version && "
        "printf '\\nFFMPEG_STUDIO_SYSTEM\\n' && "
        "uname -s && "
        "(getconf _NPROCESSORS_ONLN 2>/dev/null || "
        "sysctl -n hw.logicalcpu 2>/dev/null || echo 1) && "
        "(awk '/MemAvailable:/ {printf \"%.0f\\n\", $2 * 1024; found=1} "
        "END {if (!found) exit 1}' /proc/meminfo 2>/dev/null || "
        "sysctl -n hw.memsize 2>/dev/null || echo 0)";

    const ProcessResult probeResult = sshService_->runRemote(alias, command);
    if (probeResult.exitCode != 0) {
        result.error = trim(probeResult.output);
        if (result.error.empty()) {
            result.error = "无法连接远程服务器，或远程服务器没有安装 FFmpeg 和 ffprobe。";
        }
        return result;
    }

    std::istringstream output(probeResult.output);
    std::string line;
    bool readingSystemInformation = false;
    std::vector<std::string> systemValues;

    while (std::getline(output, line)) {
        if (line.starts_with("ffmpeg version") && result.capabilities.ffmpegVersion.empty()) {
            result.capabilities.ffmpegVersion = line;
        }

        if (line == "FFMPEG_STUDIO_SYSTEM") {
            readingSystemInformation = true;
            continue;
        }

        if (readingSystemInformation && !trim(line).empty()) {
            systemValues.push_back(trim(line));
        }
    }

    const auto hasEncoder = [&](std::string_view encoder) {
        return probeResult.output.find(encoder) != std::string::npos;
    };

    result.capabilities.libx264 = hasEncoder("libx264");
    result.capabilities.libx265 = hasEncoder("libx265");
    result.capabilities.libsvtav1 = hasEncoder("libsvtav1");
    result.capabilities.libaomAv1 = hasEncoder("libaom-av1");
    result.capabilities.libopus = hasEncoder("libopus");
    result.capabilities.libvorbis = hasEncoder("libvorbis");
    result.capabilities.ffprobeAvailable = hasEncoder("ffprobe version");

    result.resources.operatingSystem =
        systemValues.empty() ? "未知" : systemValues[0];

    if (systemValues.size() > 1) {
        try {
            result.resources.logicalCores = std::max(
                1U, static_cast<unsigned int>(std::stoul(systemValues[1])));
        } catch (const std::exception&) {
            result.resources.logicalCores = 0;
        }
    }

    if (systemValues.size() > 2) {
        try {
            result.resources.availableMemoryBytes =
                static_cast<std::uint64_t>(std::stoull(systemValues[2]));
        } catch (const std::exception&) {
            result.resources.availableMemoryBytes = 0;
        }
    }

    const std::vector<std::pair<std::string_view, bool*>> hardwareEncoders{
        {"h264_nvenc", &result.capabilities.h264Nvenc},
        {"hevc_nvenc", &result.capabilities.hevcNvenc},
        {"av1_nvenc", &result.capabilities.av1Nvenc},
        {"h264_qsv", &result.capabilities.h264Qsv},
        {"hevc_qsv", &result.capabilities.hevcQsv},
        {"av1_qsv", &result.capabilities.av1Qsv},
        {"h264_amf", &result.capabilities.h264Amf},
        {"hevc_amf", &result.capabilities.hevcAmf},
        {"av1_amf", &result.capabilities.av1Amf}
    };

    for (const auto& [encoderName, available] : hardwareEncoders) {
        *available = hasEncoder(encoderName);
        if (!*available || !testHardware) {
            if (std::string_view(encoderName).ends_with("_nvenc")) {
                result.capabilities.nvencProbeDetails += std::string(encoderName) + ": " +
                    (*available ? "FFmpeg 已提供该编码器，尚未检测硬件" :
                                  "当前 FFmpeg 构建未提供该编码器") + "\n";
            }
            continue;
        }

        const std::string testCommand =
            "ffmpeg -hide_banner -loglevel error -nostdin "
            "-f lavfi -i testsrc2=size=640x360:rate=30:duration=1 "
            "-frames:v 3 -an -pix_fmt yuv420p -c:v " + std::string(encoderName) +
            " -f null - 2>&1";

        const ProcessResult hardwareTest = sshService_->runRemote(alias, testCommand);
        *available = hardwareTest.exitCode == 0;
        if (std::string_view(encoderName).ends_with("_nvenc")) {
            std::string status = *available ? "检测成功" : trim(hardwareTest.output);
            if (status.size() > 260) status.resize(260);
            result.capabilities.nvencProbeDetails +=
                std::string(encoderName) + ": " + (status.empty() ? "检测失败" : status) + "\n";
        }
    }

    if (result.capabilities.ffmpegVersion.empty() ||
        !result.capabilities.ffprobeAvailable) {
        result.error = "远程服务器上的 FFmpeg 或 ffprobe 无法正常运行。";
        return result;
    }

    result.success = true;
    return result;
}

EncoderCapabilities FfmpegService::probeCapabilities(bool testHardware) const {
    EncoderCapabilities capabilities;
    const ProcessResult version = runProcess({ffmpegPath_, "-version"});
    if (version.exitCode == 0) {
        std::istringstream stream(version.output);
        std::getline(stream, capabilities.ffmpegVersion);
    }
    const ProcessResult encoders = runProcess({ffmpegPath_, "-hide_banner", "-encoders"});
    if (encoders.exitCode == 0) {
        capabilities.libx264 = encoders.output.find("libx264") != std::string::npos;
        capabilities.libx265 = encoders.output.find("libx265") != std::string::npos;
        capabilities.libsvtav1 = encoders.output.find("libsvtav1") != std::string::npos;
        capabilities.libaomAv1 = encoders.output.find("libaom-av1") != std::string::npos;
        capabilities.libopus = encoders.output.find("libopus") != std::string::npos;
        capabilities.libvorbis = encoders.output.find("libvorbis") != std::string::npos;
        const auto available = [&](const std::string& name) { return encoders.output.find(name) != std::string::npos; };
        const std::vector<std::pair<std::string, bool*>> hardware{
            {"h264_nvenc", &capabilities.h264Nvenc}, {"hevc_nvenc", &capabilities.hevcNvenc},
            {"av1_nvenc", &capabilities.av1Nvenc}, {"h264_qsv", &capabilities.h264Qsv},
            {"hevc_qsv", &capabilities.hevcQsv}, {"av1_qsv", &capabilities.av1Qsv},
            {"h264_amf", &capabilities.h264Amf}, {"hevc_amf", &capabilities.hevcAmf},
            {"av1_amf", &capabilities.av1Amf}};
        for (const auto& [name, flag] : hardware) {
            const bool encoderListed = available(name);
            std::string detail;
            *flag = encoderListed && (!testHardware || probeHardwareEncoder(name, &detail));
            if (name.ends_with("_nvenc")) {
                const std::string status = !encoderListed
                    ? "当前 FFmpeg 构建未提供该编码器"
                    : !testHardware ? "FFmpeg 已提供该编码器，尚未检测硬件"
                    : (*flag ? "检测成功" : detail);
                capabilities.nvencProbeDetails += name + ": " + status + "\n";
            }
        }
    }
    capabilities.ffprobeAvailable = runProcess({ffprobePath_, "-version"}).exitCode == 0;
    return capabilities;
}

bool FfmpegService::probeHardwareEncoder(const std::string& encoder, std::string* detail) const {
    // Probe a practical 640x360 input. Some hardware encoders reject very small frames.
    const ProcessResult result = runProcess({ffmpegPath_, "-hide_banner", "-loglevel", "error",
        "-nostdin", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30:duration=1",
        "-frames:v", "3", "-an", "-pix_fmt", "yuv420p",
        "-c:v", encoder, "-f", "null", "-"});
    if (detail != nullptr) {
        if (result.exitCode == 0) {
            *detail = "检测成功";
        } else {
            *detail = trim(result.output);
            for (char& character : *detail) {
                if (character == '\r' || character == '\n' || character == '\t')
                    character = ' ';
            }
            if (detail->size() > 260) detail->resize(260);
            if (detail->empty()) *detail = "设备初始化失败，FFmpeg 退出码：" +
                std::to_string(result.exitCode);
        }
    }
    return result.exitCode == 0;
}

std::optional<double> FfmpegService::probeDuration(const std::string& inputPath) const {
    const ProcessResult result = runProcess({ffprobePath_, "-v", "error", "-show_entries",
        "format=duration", "-of", "default=noprint_wrappers=1:nokey=1", inputPath});
    if (result.exitCode != 0) return std::nullopt;
    try {
        const double duration = std::stod(trim(result.output));
        if (std::isfinite(duration) && duration > 0.0) return duration;
    } catch (...) {}
    return std::nullopt;
}

bool FfmpegService::isAlreadyProcessed(const std::string& inputPath,
                                       const ConversionSettings& settings) const {
    const std::filesystem::path path = pathFromUtf8(inputPath);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return false;
    const ProcessResult result = runProcess({ffprobePath_, "-v", "error", "-show_entries",
        "format_tags=comment", "-of", "default=noprint_wrappers=1", inputPath});
    if (result.exitCode != 0) return false;
    const std::string currentPrefix = "VideoCompressionWorkbench:v3:" + settingsFingerprint(settings) + ":";
    const std::string legacyV2Prefix = supportsLegacyMarker(settings)
        ? "FFmpegStudio:v2:" + legacySettingsFingerprint(settings) + ":" : std::string{};
    const std::string legacyV1Marker = supportsLegacyMarker(settings)
        ? "FFmpegStudio:v1:" + legacySettingsFingerprint(settings) : std::string{};
    return result.output.find(currentPrefix) != std::string::npos ||
           (!legacyV2Prefix.empty() && result.output.find(legacyV2Prefix) != std::string::npos) ||
           (!legacyV1Marker.empty() && result.output.find(legacyV1Marker) != std::string::npos);
}

bool FfmpegService::isOutputForSourceAlreadyProcessed(const std::string& outputPath,
                                                       const ConversionSettings& settings) const {
    const std::filesystem::path path = pathFromUtf8(outputPath);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return false;
    const ProcessResult result = runProcess({ffprobePath_, "-v", "error", "-show_entries",
        "format_tags=comment", "-of", "default=noprint_wrappers=1", outputPath});
    if (result.exitCode != 0) return false;
    if (result.output.find(processingMarker(settings)) != std::string::npos) return true;
    if (!supportsLegacyMarker(settings)) return false;
    const std::string legacyMarker = "FFmpegStudio:v2:" +
        legacySettingsFingerprint(settings) + ":" + sourceFingerprint(settings.markerSourcePath.empty()
            ? settings.inputPath : settings.markerSourcePath);
    return result.output.find(legacyMarker) != std::string::npos;
}

bool FfmpegService::verifyOutput(const std::string& path, VideoCodec codec,
                                 const std::string& expectedMarker,
                                 const std::string& expectedAudioCodec,
                                 OutputResolution resolution) const {
    const ProcessResult result = runProcess({ffprobePath_, "-v", "error", "-show_entries",
        "format=format_name:format_tags=comment:stream=codec_type,codec_name,width,height",
        "-of", "default=noprint_wrappers=1", path});
    if (result.exitCode != 0) return false;
    const std::string extension = lower(pathToUtf8(pathFromUtf8(path).extension()));
    const std::string format = result.output;
    const bool hasVideo = format.find("codec_name=" + expectedStreamCodec(codec)) != std::string::npos;
    bool expectedContainer = false;
    if (extension == ".mp4" || extension == ".m4v" || extension == ".mov" || extension == ".3gp")
        expectedContainer = format.find("mov,") != std::string::npos || format.find("mp4") != std::string::npos;
    else if (extension == ".mkv") expectedContainer = format.find("matroska") != std::string::npos;
    else if (extension == ".webm") expectedContainer = format.find("webm") != std::string::npos;
    else if (extension == ".avi") expectedContainer = format.find("avi") != std::string::npos;
    else if (extension == ".wmv") expectedContainer = format.find("asf") != std::string::npos;
    else if (extension == ".flv") expectedContainer = format.find("flv") != std::string::npos;
    else if (extension == ".ts" || extension == ".m2ts" || extension == ".mts")
        expectedContainer = format.find("mpegts") != std::string::npos;
    else if (extension == ".mpg" || extension == ".mpeg" || extension == ".vob")
        expectedContainer = format.find("mpeg") != std::string::npos;
    else if (extension == ".ogv") expectedContainer = format.find("ogg") != std::string::npos;
    const bool hasAudio = format.find("codec_type=audio") != std::string::npos;
    const bool audioMatches = expectedAudioCodec == "none"
        ? !hasAudio
        : expectedAudioCodec == "copy" ||
            !hasAudio ||
            format.find("codec_name=" + expectedAudioCodec) != std::string::npos;
    const bool hasMarker = format.find(expectedMarker) != std::string::npos;
    const bool resolutionMatches = outputResolutionMatches(format, resolution);
    return hasVideo && expectedContainer && audioMatches && hasMarker && resolutionMatches;
}

ConversionResult FfmpegService::convertRemote(
    const ConversionSettings& settings,
    const EncoderCapabilities& capabilities,
    const ProgressCallback& onProgress,
    const std::atomic_bool* cancelRequested,
    const std::atomic_bool* pauseRequested) const {
    namespace fs = std::filesystem;

    ConversionResult result;
    result.outputPath = settings.outputPath;

    std::error_code filesystemError;
    const fs::path inputPath = pathFromUtf8(settings.inputPath);
    const fs::path outputPath = pathFromUtf8(settings.outputPath);
    const std::string outputExtension =
        outputVideoExtension(settings.outputContainer, settings.inputPath);

    if (settings.remoteHost.empty() || !fs::is_regular_file(inputPath, filesystemError)) {
        result.message = "请选择有效的视频文件和远程服务器。";
        return result;
    }
    if (outputExtension.empty() ||
        lower(pathToUtf8(outputPath.extension())) != outputExtension) {
        result.message = "输出文件扩展名与所选容器格式不一致。";
        return result;
    }
    if (!capabilities.supports(settings.codec, settings.backend) ||
        !capabilities.ffprobeAvailable) {
        result.message = "远程服务器不支持当前选择的视频编码器，或没有安装 ffprobe。";
        return result;
    }
    if (!containerSupportsVideoCodec(outputExtension, settings.codec)) {
        result.message = "所选视频编码格式与目标容器不兼容。";
        return result;
    }

    const std::string audioEncoder = settings.audioMode == AudioMode::Encode
        ? audioEncoderForExtension(outputExtension, capabilities) : std::string{};
    if (settings.audioMode == AudioMode::Encode && audioEncoder.empty()) {
        result.message = "远程 FFmpeg 缺少该容器所需的音频编码器。";
        return result;
    }

    result.inputSizeBytes = fs::file_size(inputPath, filesystemError);
    if (filesystemError || result.inputSizeBytes == 0) {
        result.message = "无法读取源文件大小，或源文件为空。";
        return result;
    }

    const fs::path outputDirectory =
        outputPath.parent_path().empty() ? fs::current_path() : outputPath.parent_path();
    fs::create_directories(outputDirectory, filesystemError);
    if (filesystemError) {
        result.message = "无法创建输出目录：" + filesystemError.message();
        return result;
    }

    if (fs::is_regular_file(outputPath, filesystemError)) {
        if (settings.skipAlreadyProcessed &&
            isOutputForSourceAlreadyProcessed(settings.outputPath, settings)) {
            result.success = true;
            result.skipped = true;
            result.message = "目标文件已经使用相同参数处理，已自动跳过。";
            result.outputSizeBytes = fs::file_size(outputPath, filesystemError);

            if (onProgress) {
                ConversionProgress progress;
                progress.fraction = 1.0;
                progress.status = "已跳过";
                progress.detail = result.message;
                onProgress(progress);
            }
            return result;
        }

        if (!settings.overwrite) {
            result.message = "输出文件已经存在。请修改输出位置或确认覆盖操作。";
            return result;
        }
    }

    const SshService& ssh = *sshService_;
    const auto quote = [](const std::string& value) {
        return SshService::shellQuote(value);
    };
    const auto runRemote = [&](const std::string& command) {
        return ssh.runRemote(settings.remoteHost, command);
    };

    ConversionProgress progress;
    progress.status = "正在连接远程服务器";
    progress.detail = "正在通过 SSH 建立安全连接";
    if (onProgress) {
        onProgress(progress);
    }

    const auto directoryResult =
        runRemote("umask 077 && mktemp -d /tmp/ffmpeg-studio-XXXXXX");
    if (directoryResult.exitCode != 0) {
        result.message = trim(directoryResult.output);
        if (result.message.empty()) {
            result.message = "无法在远程服务器创建临时工作目录。";
        }
        return result;
    }

    const std::string remoteDirectory = trim(directoryResult.output);
    if (remoteDirectory.empty() || remoteDirectory.front() != '/') {
        result.message = "远程服务器返回了无效的临时目录。";
        return result;
    }

    // Keep the source extension so FFmpeg can select the expected input demuxer.
    std::string sourceExtension = lower(pathToUtf8(inputPath.extension()));
    if (sourceExtension.empty()) {
        sourceExtension = ".bin";
    }

    const std::string remoteInput = remoteDirectory + "/input" + sourceExtension;
    const std::string remoteOutput = remoteDirectory + "/output" + outputExtension;
    const std::string processIdFile = remoteDirectory + "/ffmpeg.pid";

    // The guard cleans up the remote workspace on success, failure, cancellation and exceptions.
    struct RemoteWorkspaceCleanup {
        const SshService& ssh;
        const std::string& alias;
        const std::string& directory;
        const std::string& processIdFile;

        ~RemoteWorkspaceCleanup() {
            const auto quoteValue = [](const std::string& value) {
                return SshService::shellQuote(value);
            };

            const std::string command =
                "if [ -r " + quoteValue(processIdFile) + " ]; then "
                "pid=$(cat " + quoteValue(processIdFile) + "); "
                "[ -n \"$pid\" ] && kill -TERM \"$pid\" 2>/dev/null || true; "
                "sleep 1; "
                "[ -n \"$pid\" ] && kill -KILL \"$pid\" 2>/dev/null || true; "
                "fi; rm -rf -- " + quoteValue(directory);

            (void)ssh.runRemote(alias, command);
        }
    } cleanup{ssh, settings.remoteHost, remoteDirectory, processIdFile};

    progress.status = "正在上传源文件";
    progress.detail = "正在将视频安全传输到远程服务器";
    if (onProgress) {
        onProgress(progress);
    }

    const auto upload = ssh.upload(
        settings.remoteHost, settings.inputPath, remoteInput, cancelRequested);
    if (upload.cancelled || (cancelRequested && cancelRequested->load())) {
        result.cancelled = true;
        result.message = "上传已取消。";
        return result;
    }
    if (upload.exitCode != 0) {
        result.message = processErrorSummary(upload.output);
        if (result.message.empty()) {
            result.message = "无法将源文件上传到远程服务器。";
        }
        return result;
    }

    // The local output marker was checked before upload; the remote input is a fresh copy.
    const std::string marker = processingMarker(settings);

    const std::string durationCommand =
        "ffprobe -v error -show_entries format=duration "
        "-of default=noprint_wrappers=1:nokey=1 " + quote(remoteInput);
    const auto durationResult = runRemote(durationCommand);

    std::optional<double> duration;
    if (durationResult.exitCode == 0) {
        try {
            const double value = std::stod(trim(durationResult.output));
            if (std::isfinite(value) && value > 0.0) {
                duration = value;
            }
        } catch (const std::exception&) {
            // Duration is optional; conversion progress remains available without it.
        }
    }

    progress.status = "正在远程编码";
    progress.detail = "正在读取远程 FFmpeg 编码进度";
    progress.durationSeconds = duration;
    if (onProgress) {
        onProgress(progress);
    }

    ConversionSettings remoteSettings = settings;
    remoteSettings.inputPath = remoteInput;
    remoteSettings.markerSourcePath = settings.inputPath;
    remoteSettings.remoteHost.clear();

    std::vector<std::string> commandArguments;
    try {
        commandArguments = buildFfmpegCommand(
            remoteSettings, capabilities, "ffmpeg", remoteOutput);
    } catch (const std::exception& exception) {
        result.message = exception.what();
        return result;
    }

    const std::string remoteCommand = joinRemoteArguments(commandArguments);
    const std::string remoteScript =
        remoteCommand + " & ffmpeg_pid=$!; "
        "printf '%s\\n' \"$ffmpeg_pid\" > " + quote(processIdFile) + "; "
        "wait \"$ffmpeg_pid\"; exitCode=$?; "
        "rm -f -- " + quote(processIdFile) + "; "
        "exit \"$exitCode\"";

    std::atomic_bool monitorFinished{false};
    std::thread processMonitor;

    if (pauseRequested != nullptr) {
        processMonitor = std::thread([&]() {
            bool remoteProcessPaused = false;

            while (!monitorFinished.load()) {
                if (cancelRequested != nullptr && cancelRequested->load()) {
                    const std::string terminateCommand =
                        "if [ -r " + quote(processIdFile) + " ]; then "
                        "pid=$(cat " + quote(processIdFile) + "); "
                        "[ -n \"$pid\" ] && kill -TERM \"$pid\" 2>/dev/null || true; "
                        "sleep 1; "
                        "[ -n \"$pid\" ] && kill -KILL \"$pid\" 2>/dev/null || true; "
                        "fi";
                    (void)runRemote(terminateCommand);
                    break;
                }

                const bool pauseRequestedNow = pauseRequested->load();
                if (pauseRequestedNow != remoteProcessPaused) {
                    const std::string signal = pauseRequestedNow ? "-STOP" : "-CONT";
                    const std::string signalCommand =
                        "if [ -r " + quote(processIdFile) + " ]; then "
                        "pid=$(cat " + quote(processIdFile) + "); "
                        "[ -n \"$pid\" ] && kill " + signal + " \"$pid\"; "
                        "else exit 1; fi";

                    if (runRemote(signalCommand).exitCode == 0) {
                        remoteProcessPaused = pauseRequestedNow;
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(120));
            }

            if (remoteProcessPaused) {
                const std::string resumeCommand =
                    "if [ -r " + quote(processIdFile) + " ]; then "
                    "pid=$(cat " + quote(processIdFile) + "); "
                    "[ -n \"$pid\" ] && kill -CONT \"$pid\" 2>/dev/null || true; "
                    "fi";
                (void)runRemote(resumeCommand);
            }
        });
    }

    const auto process = ssh.runRemote(
        settings.remoteHost,
        remoteScript,
        cancelRequested,
        [&](std::string_view line) {
            if (!onProgress) {
                return;
            }

            const std::size_t separator = line.find('=');
            if (separator == std::string_view::npos) {
                return;
            }

            const std::string_view key = line.substr(0, separator);
            const std::string_view value = line.substr(separator + 1);

            if (key == "frame") {
                progress.frame = parseUnsigned(value);
            } else if (key == "fps") {
                progress.fps = parseNumber(value);
            } else if (key == "out_time_us" || key == "out_time_ms") {
                if (const auto microseconds = parseNumber(value)) {
                    progress.currentTimeSeconds = *microseconds / 1000000.0;
                }
            } else if (key == "out_time" && !progress.currentTimeSeconds) {
                progress.currentTimeSeconds = parseTimestamp(value);
            } else if (key == "speed") {
                std::string speed(value);
                if (!speed.empty() && speed.back() == 'x') {
                    speed.pop_back();
                }
                progress.speed = parseNumber(speed);
            } else if (key == "bitrate" && value != "N/A") {
                progress.bitrate = std::string(value);
            } else if (key == "total_size") {
                progress.totalSizeBytes = parseUnsigned(value);
            } else if (key == "progress") {
                if (value == "end") {
                    progress.fraction = 0.995;
                    progress.status = "正在检查远程输出";
                    progress.detail = "正在检查容器和编码信息";
                    onProgress(progress);
                } else if (value == "continue") {
                    if (duration && progress.currentTimeSeconds) {
                        progress.durationSeconds = duration;
                        progress.fraction = std::clamp(
                            *progress.currentTimeSeconds / *duration, 0.0, 0.995);
                        if (progress.speed && *progress.speed > 0.0) {
                            progress.etaSeconds = std::max(
                                0.0, (*duration - *progress.currentTimeSeconds) / *progress.speed);
                        }
                    }

                    progress.status = "正在远程编码";
                    progress.detail = formatProgressDetail(progress);
                    onProgress(progress);
                }
            }
        });

    monitorFinished.store(true);
    if (processMonitor.joinable()) {
        processMonitor.join();
    }

    if (process.cancelled || (cancelRequested != nullptr && cancelRequested->load())) {
        result.cancelled = true;
        result.message = "远程转换已取消。";
        return result;
    }
    if (process.exitCode != 0) {
        result.message = processErrorSummary(process.output);
        if (result.message.empty()) {
            result.message = "远程 FFmpeg 转换失败。";
        }
        return result;
    }

    progress.status = "正在验证远程输出";
    progress.detail = "正在检查容器、视频编码、音频编码、输出分辨率和处理标记";
    if (onProgress) {
        onProgress(progress);
    }

    const std::string probeCommand =
        "ffprobe -v error -show_entries "
        "format=format_name:format_tags=comment:stream=codec_type,codec_name,width,height "
        "-of default=noprint_wrappers=1 " + quote(remoteOutput);
    const auto probe = runRemote(probeCommand);
    if (probe.exitCode != 0 ||
        !outputDescriptionMatches(probe.output, remoteOutput, settings.codec, marker,
                                  expectedAudioCodecForSettings(settings.audioMode, audioEncoder),
                                  settings.outputResolution)) {
        result.message = "远程输出未通过容器格式、视频编码、音频编码、输出分辨率或处理标记检查。";
        return result;
    }

    const auto sizeResult = runRemote("wc -c < " + quote(remoteOutput));
    const std::uint64_t remoteOutputSize =
        sizeResult.exitCode == 0 ? parseRemoteSize(sizeResult.output) : 0;
    if (remoteOutputSize == 0) {
        result.message = "远程输出文件为空或无法读取大小。";
        return result;
    }

    progress.status = "正在下载转换结果";
    progress.detail = "远程编码完成，正在将文件传回本机";
    if (onProgress) {
        onProgress(progress);
    }

    const fs::path temporaryOutput = makeTemporaryPath(outputPath);
    const auto download = ssh.download(
        settings.remoteHost, remoteOutput, pathToUtf8(temporaryOutput), cancelRequested);

    if (download.cancelled || (cancelRequested != nullptr && cancelRequested->load())) {
        fs::remove(temporaryOutput, filesystemError);
        result.cancelled = true;
        result.message = "下载已取消。";
        return result;
    }
    if (download.exitCode != 0 || !fs::is_regular_file(temporaryOutput, filesystemError)) {
        fs::remove(temporaryOutput, filesystemError);
        result.message = processErrorSummary(download.output);
        if (result.message.empty()) {
            result.message = "无法下载远程转换结果。";
        }
        return result;
    }

    result.outputSizeBytes = fs::file_size(temporaryOutput, filesystemError);
    if (filesystemError || result.outputSizeBytes != remoteOutputSize) {
        fs::remove(temporaryOutput, filesystemError);
        result.message = "下载文件大小与远程输出不一致，已放弃保存。";
        return result;
    }

    std::string replaceError;
    if (!replaceFile(temporaryOutput, outputPath, settings.overwrite, replaceError)) {
        fs::remove(temporaryOutput, filesystemError);
        result.message = replaceError;
        return result;
    }

    result.success = true;
    result.message = "远程转换完成，文件已自动下载到本机。";

    if (onProgress) {
        progress.fraction = 1.0;
        progress.status = "转换完成";
        progress.detail = result.message;
        onProgress(progress);
    }

    return result;
}

ConversionResult FfmpegService::convert(const ConversionSettings& settings,
                                        const EncoderCapabilities& capabilities,
                                        const ProgressCallback& onProgress,
                                        const std::atomic_bool* cancelRequested,
                                        const std::atomic_bool* pauseRequested) const {
    namespace fs = std::filesystem;
    ConversionResult result;
    result.outputPath = settings.outputPath;
    std::error_code filesystemError;
    const fs::path inputPath = pathFromUtf8(settings.inputPath);
    const fs::path outputPath = pathFromUtf8(settings.outputPath);
    if (settings.inputPath.empty() || !fs::is_regular_file(inputPath, filesystemError)) {
        result.message = "请选择一个存在的视频文件。";
        return result;
    }
    const std::string expectedExtension = outputVideoExtension(settings.outputContainer, settings.inputPath);
    if (settings.outputPath.empty() || expectedExtension.empty() ||
        lower(pathToUtf8(outputPath.extension())) != expectedExtension) {
        result.message = settings.outputContainer == OutputContainer::MP4
            ? "MP4 输出文件必须使用 .mp4 扩展名。"
            : "输出文件扩展名必须与源文件的容器格式一致。";
        return result;
    }
    if (!settings.remoteHost.empty())
        return convertRemote(settings, capabilities, onProgress, cancelRequested, pauseRequested);
    if (!capabilities.supports(settings.codec, settings.backend)) {
        result.message = "当前 FFmpeg 环境或硬件设备不支持所选编码器。";
        return result;
    }
    const std::string outputExtension = expectedExtension;
    if (!containerSupportsVideoCodec(outputExtension, settings.codec)) {
        result.message = "所选视频编码格式与目标容器不兼容。请更换编码格式或选择 MP4 输出。";
        return result;
    }
    const std::string expectedAudioCodec = settings.audioMode == AudioMode::Encode
        ? audioEncoderForExtension(outputExtension, capabilities) : std::string{};
    if (settings.audioMode == AudioMode::Encode && expectedAudioCodec.empty()) {
        result.message = "当前 FFmpeg 未提供该容器所需的音频编码器。";
        return result;
    }
    if (!capabilities.ffprobeAvailable) {
        result.message = "当前环境未检测到 ffprobe。请安装与 FFmpeg 配套的工具后重新启动程序。";
        return result;
    }
    if (settings.crf < 0 || settings.crf > crfMaximum(settings.codec)) {
        result.message = "CRF 数值超出当前编码格式的范围。";
        return result;
    }
    if (settings.audioMode == AudioMode::Encode &&
        (settings.audioBitrateKbps < 64 || settings.audioBitrateKbps > 320)) {
        result.message = "音频码率必须介于 64 到 320 kbps 之间。";
        return result;
    }
    if (settings.skipAlreadyProcessed && fs::is_regular_file(outputPath, filesystemError) &&
        isOutputForSourceAlreadyProcessed(settings.outputPath, settings)) {
        result.success = true;
        result.skipped = true;
        result.message = "目标文件已经使用相同参数处理，已自动跳过。";
        result.inputSizeBytes = fs::file_size(inputPath, filesystemError);
        result.outputSizeBytes = fs::file_size(outputPath, filesystemError);
        if (onProgress) {
            ConversionProgress progress;
            progress.fraction = 1.0;
            progress.status = "已跳过";
            progress.detail = result.message;
            onProgress(progress);
        }
        return result;
    }
    if (settings.skipAlreadyProcessed && isAlreadyProcessed(settings.inputPath, settings)) {
        const fs::path outputDirectory = outputPath.parent_path().empty() ? fs::current_path() : outputPath.parent_path();
        fs::create_directories(outputDirectory, filesystemError);
        if (filesystemError) {
            result.message = "无法创建输出目录：" + filesystemError.message();
            return result;
        }
        const fs::path canonicalInput = fs::weakly_canonical(inputPath, filesystemError);
        filesystemError.clear();
        const fs::path canonicalOutput = fs::weakly_canonical(outputPath, filesystemError);
        if (!filesystemError && canonicalInput != canonicalOutput) {
            const auto option = settings.overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none;
            fs::copy_file(inputPath, outputPath, option, filesystemError);
            if (filesystemError) {
                result.message = "视频已经使用相同参数处理，但无法复制到输出位置：" + filesystemError.message();
                return result;
            }
        }
        result.success = true;
        result.skipped = true;
        result.message = "视频已经使用相同参数处理，已直接复用现有文件。";
        result.inputSizeBytes = fs::file_size(inputPath, filesystemError);
        result.outputSizeBytes = fs::file_size(outputPath, filesystemError);
        if (onProgress) {
            ConversionProgress progress;
            progress.fraction = 1.0;
            progress.status = "已跳过";
            progress.detail = result.message;
            onProgress(progress);
        }
        return result;
    }

    const fs::path outputDirectory = outputPath.parent_path().empty() ? fs::current_path() : outputPath.parent_path();
    fs::create_directories(outputDirectory, filesystemError);
    if (filesystemError || !fs::is_directory(outputDirectory, filesystemError)) {
        result.message = "无法创建输出目录：" + filesystemError.message();
        return result;
    }
    if (fs::exists(outputPath, filesystemError) && !settings.overwrite) {
        result.message = "输出文件已经存在。请修改输出位置或确认覆盖操作。";
        return result;
    }
    const fs::path canonicalInput = fs::weakly_canonical(inputPath, filesystemError);
    filesystemError.clear();
    const fs::path canonicalOutput = fs::weakly_canonical(outputPath, filesystemError);
    if (!filesystemError && canonicalInput == canonicalOutput) {
        result.message = "输入文件和输出文件不能使用同一个路径。";
        return result;
    }

    result.inputSizeBytes = fs::file_size(inputPath, filesystemError);
    const fs::path temporaryPath = makeTemporaryPath(outputPath);
    const auto duration = probeDuration(settings.inputPath);
    ConversionProgress progressState;
    progressState.status = "正在准备编码";
    progressState.detail = "正在检查输入文件和编码参数";
    progressState.durationSeconds = duration;
    if (onProgress) onProgress(progressState);

    std::vector<std::string> command;
    const std::string marker = processingMarker(settings);
    try {
        command = buildFfmpegCommand(settings, capabilities, ffmpegPath_, pathToUtf8(temporaryPath));
    } catch (const std::exception& exception) {
        result.message = exception.what();
        return result;
    }

    progressState.status = "正在转换视频";
    progressState.detail = duration ? "正在读取 FFmpeg 编码进度" : "正在编码，当前环境无法读取视频时长";
    if (onProgress) onProgress(progressState);
    const ProcessResult process = runProcess(command, cancelRequested, [&](std::string_view line) {
        if (!onProgress) return;
        const auto separator = line.find('=');
        if (separator == std::string_view::npos) return;
        const std::string_view key = line.substr(0, separator);
        const std::string_view value = line.substr(separator + 1);
        if (key == "frame") progressState.frame = parseUnsigned(value);
        else if (key == "fps") progressState.fps = parseNumber(value);
        else if (key == "out_time_us" || key == "out_time_ms") {
            if (const auto microseconds = parseNumber(value))
                progressState.currentTimeSeconds = *microseconds / 1'000'000.0;
        } else if (key == "out_time" && !progressState.currentTimeSeconds) {
            progressState.currentTimeSeconds = parseTimestamp(value);
        } else if (key == "speed") {
            std::string speedText(value);
            if (!speedText.empty() && speedText.back() == 'x') speedText.pop_back();
            progressState.speed = parseNumber(speedText);
        } else if (key == "bitrate") {
            if (value != "N/A") progressState.bitrate = std::string(value);
        } else if (key == "total_size") progressState.totalSizeBytes = parseUnsigned(value);
        else if (key == "progress") {
            if (value == "end") {
                progressState.fraction = 0.995;
                progressState.etaSeconds = 0.0;
                progressState.status = "正在检查文件";
                progressState.detail = "正在验证输出文件并保存到目标位置";
                onProgress(progressState);
            } else if (value == "continue") {
                if (duration && progressState.currentTimeSeconds) {
                    progressState.durationSeconds = duration;
                    progressState.fraction = std::clamp(*progressState.currentTimeSeconds / *duration, 0.0, 0.995);
                    if (progressState.speed && *progressState.speed > 0.0) {
                        progressState.etaSeconds = std::max(0.0,
                            (*duration - *progressState.currentTimeSeconds) / *progressState.speed);
                    }
                }
                progressState.status = "正在转换视频";
                progressState.detail = formatProgressDetail(progressState);
                onProgress(progressState);
            }
        }
    }, pauseRequested);

    if (process.cancelled || (cancelRequested && cancelRequested->load())) {
        fs::remove(temporaryPath, filesystemError);
        result.cancelled = true;
        result.message = "转换已取消。";
        return result;
    }
    if (process.exitCode != 0) {
        fs::remove(temporaryPath, filesystemError);
        result.message = processErrorSummary(process.output);
        return result;
    }
    if (!verifyOutput(pathToUtf8(temporaryPath), settings.codec, marker,
                      expectedAudioCodecForSettings(settings.audioMode, expectedAudioCodec),
                      settings.outputResolution)) {
        fs::remove(temporaryPath, filesystemError);
        result.message = "FFmpeg 已结束，但输出文件未通过容器格式、视频编码、音频编码、输出分辨率或处理标记检查。";
        return result;
    }

    std::string replaceError;
    if (!replaceFile(temporaryPath, outputPath, settings.overwrite, replaceError)) {
        fs::remove(temporaryPath, filesystemError);
        result.message = replaceError;
        return result;
    }
    result.success = true;
    result.message = "转换完成。";
    result.outputSizeBytes = fs::file_size(outputPath, filesystemError);
    if (onProgress) {
        progressState.fraction = 1.0;
        progressState.status = "转换完成";
        progressState.detail = "输出文件已保存并通过编码检查";
        onProgress(progressState);
    }
    return result;
}

} // namespace ffmpegui
