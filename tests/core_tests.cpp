#include "core/app_controller.hpp"
#include "core/conversion.hpp"
#include "core/process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void waitForScan(ffmpegui::AppController& controller, std::size_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    do {
        const auto snapshot = controller.snapshot();
        if (!snapshot.scanningInputs && snapshot.totalTasks == expected) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Input scanner did not finish within the deadline.");
}

bool containsPair(const std::vector<std::string>& command,
                  const std::string& option, const std::string& value) {
    for (std::size_t index = 0; index + 1 < command.size(); ++index) {
        if (command[index] == option && command[index + 1] == value) {
            return true;
        }
    }
    return false;
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

} // namespace

int main() {
    try {
        using namespace ffmpegui;
        EncoderCapabilities capabilities;
        capabilities.libx264 = true;
        capabilities.libx265 = true;
        capabilities.libvpxVp9 = true;
        capabilities.libvvenc = true;
        capabilities.libsvtav1 = true;
        capabilities.libaomAv1 = true;
        capabilities.libopus = true;
        capabilities.libvorbis = true;
        capabilities.h264Nvenc = true;
        capabilities.hevcNvenc = true;
        capabilities.av1Nvenc = true;
        capabilities.h264Qsv = true;
        capabilities.hevcQsv = true;
        capabilities.av1Qsv = true;
        capabilities.h264Amf = true;
        capabilities.hevcAmf = true;
        capabilities.av1Amf = true;

        require(compressionLevelCount() == 8, "The interface must expose eight compression levels.");
        require(parseTrimTime("00:01:23.500") == std::optional<double>{83.5},
                "Trim times must support HH:MM:SS.mmm.");
        require(parseTrimTime("11:03.250") == std::optional<double>{663.25},
                "Trim times must support MM:SS.mmm.");
        require(parseTrimTime("27.125") == std::optional<double>{27.125},
                "Trim times must support seconds and milliseconds.");
        require(!parseTrimTime("1:70:00") && !parseTrimTime("-1") &&
                !parseTrimTime("1:2:3:4") && !parseTrimTime("0:1e3") &&
                !parseTrimTime("1.2.3") && !parseTrimTime(""),
                "Malformed trim times must be rejected.");
        require(formatTrimTime(83.567) == "00:01:23.567",
                "Trim formatter must retain millisecond precision.");
        require(validTrimRange(2.0, 4.0) && validTrimRange(0.0, -1.0) &&
                !validTrimRange(4.0, 2.0) && !validTrimRange(1.0, 1.0) &&
                !validTrimRange(-1.0, 2.0),
                "Trim ranges must be validated before encoding.");
        const auto sortedCodecs = availableVideoCodecs(capabilities);
        require(sortedCodecs == std::vector<VideoCodec>{
                VideoCodec::H264, VideoCodec::H265, VideoCodec::VP9,
                VideoCodec::AV1, VideoCodec::H266
            }, "Video codecs must be ordered by their introduction dates.");
        require(crfMaximum(VideoCodec::VP9) == 63 &&
                crfMaximum(VideoCodec::H266) == 63,
                "VP9 CRF and VVC QP ranges must accept values up to 63.");
        require(!capabilities.supports(VideoCodec::VP9, EncoderBackend::NVENC) &&
                !capabilities.supports(VideoCodec::H266, EncoderBackend::QSV),
                "VP9/VVC should not advertise unsupported hardware encoder backends.");
        const std::vector<std::string> expectedX264{
            "ultrafast", "veryfast", "fast", "medium", "slow", "slower", "veryslow", "placebo"};
        const std::vector<std::string> expectedSvt{"13", "11", "10", "8", "6", "4", "2", "0"};
        const std::vector<std::string> expectedAom{"8", "7", "6", "4", "3", "2", "1", "0"};
        const std::vector<std::string> expectedNvenc{"p1", "p2", "p3", "p4", "p5", "p6", "p7", "p7"};
        const std::vector<std::string> expectedQsv{
            "veryfast", "faster", "fast", "medium",
            "slow", "slower", "veryslow", "veryslow"
        };

        ConversionSettings settings;
        settings.inputPath = "input file.mov";
        settings.outputPath = "output.mp4";
        settings.codec = VideoCodec::H264;
        settings.backend = EncoderBackend::CPU;
        settings.crf = 23;
        settings.compression = CompressionLevel::Balanced;
        settings.audioBitrateKbps = 192;
        auto command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
        require(containsPair(command, "-c:v", "libx264"), "H.264 must use libx264 on CPU.");
        require(containsPair(command, "-crf", "23"), "H.264 CRF must be passed to FFmpeg.");
        require(containsPair(command, "-preset", "medium"), "Balanced H.264 must use medium preset.");
        require(containsPair(command, "-map", "0:a?"), "Optional audio streams must be mapped.");
        require(containsPair(command, "-c:a", "aac"), "Audio must be encoded as AAC.");
        require(containsPair(command, "-b:a", "192k"), "The selected audio bitrate must be passed to FFmpeg.");
        require(
            !containsPair(command, "-threads:v", "0"),
            "An unset thread limit must not add an invalid thread option.");
        settings.trimStartSeconds = 63.250;
        settings.trimEndSeconds = 68.875;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
        const auto findArgument = [](const std::vector<std::string>& arguments,
                                     std::string_view option) {
            const auto position = std::find(arguments.begin(), arguments.end(), option);
            return position == arguments.end()
                ? arguments.size() : static_cast<std::size_t>(position - arguments.begin());
        };
        require(containsPair(command, "-ss", "00:01:03.250") &&
                containsPair(command, "-t", "00:00:05.625"),
                "Trim duration must be calculated relative to the source start.");
        require(findArgument(command, "-ss") < findArgument(command, "-i") &&
                findArgument(command, "-i") < findArgument(command, "-t"),
                "FFmpeg must seek before opening the input and limit duration on output.");
        settings.trimStartSeconds = 0.0;
        settings.trimEndSeconds = -1.0;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
        require(findArgument(command, "-ss") == command.size() &&
                findArgument(command, "-t") == command.size(),
                "Untrimmed jobs must retain their previous FFmpeg invocation.");
        settings.trimStartSeconds = 10.0;
        settings.trimEndSeconds = 9.0;
        bool rejectedTrim = false;
        try { (void)buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4"); }
        catch (const std::invalid_argument&) { rejectedTrim = true; }
        require(rejectedTrim, "End times earlier than start times must fail before FFmpeg runs.");
        settings.trimStartSeconds = 0.0;
        settings.trimEndSeconds = -1.0;

        settings.codec = VideoCodec::AV1;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.webm");
        require(containsPair(command, "-c:a", "libopus"), "WebM output must use Opus audio.");
        settings.codec = VideoCodec::H264;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mpg");
        require(containsPair(command, "-c:a", "mp2"), "MPEG program streams must use MP2 audio.");
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.wmv");
        require(containsPair(command, "-c:a", "wmav2"), "ASF/WMV output must use WMA audio.");
        bool rejectedWebmH264 = false;
        try { (void)buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.webm"); }
        catch (const std::invalid_argument&) { rejectedWebmH264 = true; }
        require(rejectedWebmH264, "WebM must reject H.264.");
        require(containerSupportsVideoCodec(".webm", VideoCodec::VP9) &&
                containerSupportsVideoCodec(".webm", VideoCodec::AV1),
                "WebM must accept VP9 and AV1.");
        require(!containerSupportsVideoCodec(".webm", VideoCodec::H266) &&
                !containerSupportsVideoCodec(".mov", VideoCodec::VP9) &&
                containerSupportsVideoCodec(".mkv", VideoCodec::H266) &&
                containerSupportsVideoCodec(".mp4", VideoCodec::H266),
                "Container compatibility must reflect supported FFmpeg combinations.");
        for (const auto [container, extension] : std::array{
                 std::pair{OutputContainer::MKV, ".mkv"},
                 std::pair{OutputContainer::WebM, ".webm"}}) {
            require(outputVideoExtension(container, "test.mp4") == extension,
                    "Explicit output containers must have stable extensions.");
        }

        settings.codec = VideoCodec::VP9;
        settings.crf = 30;
        settings.backend = EncoderBackend::CPU;
        settings.outputContainer = OutputContainer::WebM;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "output.webm");
        require(containsPair(command, "-c:v", "libvpx-vp9") &&
                containsPair(command, "-b:v", "0") &&
                containsPair(command, "-crf", "30") &&
                containsPair(command, "-row-mt", "1"),
                "VP9 must use constant-quality libvpx encoding.");
        require(containsPair(command, "-c:a", "libopus"),
                "WebM output must use a compatible audio codec.");
        settings.compression = CompressionLevel::Extreme;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "output.webm");
        require(containsPair(command, "-deadline", "best") &&
                containsPair(command, "-cpu-used", "0"),
                "Extreme VP9 compression must select the best available coding effort.");
        settings.compression = CompressionLevel::Balanced;
        settings.codec = VideoCodec::H266;
        settings.crf = 32;
        settings.outputContainer = OutputContainer::MKV;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "output.mkv");
        require(containsPair(command, "-c:v", "libvvenc") &&
                containsPair(command, "-qp", "32") &&
                containsPair(command, "-pix_fmt", "yuv420p10le") &&
                containsPair(command, "-preset", "medium"),
                "H.266 must use libvvenc QP and a valid 10-bit pixel format.");
        settings.codec = VideoCodec::H264;
        settings.outputContainer = OutputContainer::MP4;
        settings.crf = 23;
        require(
            std::any_of(
                command.begin(), command.end(),
                [](const std::string& argument) {
                    return argument.find("VideoCompressionWorkbench:v3:") != std::string::npos;
                }),
                "The output must carry a parameter marker for repeat-processing detection.");

        for (int index = 0; index < compressionLevelCount(); ++index) {
            settings.codec = VideoCodec::H264;
            settings.backend = EncoderBackend::CPU;
            settings.compression = static_cast<CompressionLevel>(index);
            command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
            require(containsPair(command, "-preset", expectedX264[static_cast<std::size_t>(index)]),
                    "Each H.264 compression level must select its corresponding CPU preset.");

            settings.codec = VideoCodec::H265;
            settings.compression = static_cast<CompressionLevel>(index);
            command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
            require(containsPair(command, "-preset", expectedX264[static_cast<std::size_t>(index)]),
                    "Each H.265 compression level must select its corresponding CPU preset.");

            settings.codec = VideoCodec::AV1;
            settings.compression = static_cast<CompressionLevel>(index);
            command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
            require(containsPair(command, "-preset", expectedSvt[static_cast<std::size_t>(index)]),
                    "Each SVT-AV1 compression level must select its corresponding preset.");
        }

        settings.codec = VideoCodec::AV1;
        settings.compression = CompressionLevel::Extreme;
        capabilities.libsvtav1 = false;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
        require(containsPair(command, "-c:v", "libaom-av1"), "libaom-av1 must be used as AV1 fallback.");
        require(containsPair(command, "-cpu-used", expectedAom.back()), "Extreme libaom AV1 must use cpu-used 0.");
        require(!containsPair(command, "-preset", "0"), "libaom AV1 must not receive an unsupported preset option.");

        capabilities.libsvtav1 = true;
        for (int index = 0; index < compressionLevelCount(); ++index) {
            settings.codec = VideoCodec::AV1;
            settings.backend = EncoderBackend::CPU;
            settings.compression = static_cast<CompressionLevel>(index);
            capabilities.libsvtav1 = false;
            command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
            require(containsPair(command, "-cpu-used", expectedAom[static_cast<std::size_t>(index)]),
                    "Each libaom AV1 compression level must select its corresponding cpu-used value.");
        }
        capabilities.libsvtav1 = true;

        for (int index = 0; index < compressionLevelCount(); ++index) {
            settings.codec = VideoCodec::H264;
            settings.backend = EncoderBackend::NVENC;
            settings.compression = static_cast<CompressionLevel>(index);
            settings.crf = 23;
            command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
            require(containsPair(command, "-c:v", "h264_nvenc"), "NVENC must select the H.264 hardware encoder.");
            require(containsPair(command, "-preset", expectedNvenc[static_cast<std::size_t>(index)]),
                    "Each NVENC compression level must select its corresponding preset.");
            require(containsPair(command, "-cq", "23"), "NVENC must receive the selected quality value.");
            require(containsPair(command, "-multipass", "fullres") == (index == 7),
                    "Only the extreme NVENC level must enable full-resolution multipass.");
        }

        for (int index = 0; index < compressionLevelCount(); ++index) {
            settings.codec = VideoCodec::H264;
            settings.backend = EncoderBackend::QSV;
            settings.compression = static_cast<CompressionLevel>(index);
            settings.threadsPerJob = 4;
            command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
            require(containsPair(command, "-c:v", "h264_qsv"), "Intel Quick Sync must select h264_qsv.");
            require(containsPair(command, "-preset", expectedQsv[static_cast<std::size_t>(index)]),
                    "Each Quick Sync compression level must map to its encoder preset.");
            require(
                containsPair(command, "-global_quality", "23"),
                "Quick Sync must receive the selected quality value.");
            require(containsPair(command, "-threads:v", "4"), "The scheduler thread budget must reach FFmpeg.");

            settings.backend = EncoderBackend::AMF;
            command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
            require(containsPair(command, "-c:v", "h264_amf"), "AMD AMF must select h264_amf.");
            require(containsPair(command, "-rc", "cqp"), "AMD AMF must use constant-QP rate control.");
            require(containsPair(command, "-qp_i", "23") && containsPair(command, "-qp_p", "23"),
                    "AMD AMF must receive the selected QP value for I and P frames.");
        }

        settings.backend = EncoderBackend::QSV;
        settings.codec = VideoCodec::H265;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
        require(containsPair(command, "-c:v", "hevc_qsv"), "Quick Sync H.265 must select hevc_qsv.");
        settings.codec = VideoCodec::AV1;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
        require(containsPair(command, "-c:v", "av1_qsv"), "Quick Sync AV1 must select av1_qsv.");
        settings.backend = EncoderBackend::AMF;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
        require(containsPair(command, "-c:v", "av1_amf"), "AMD AMF AV1 must select av1_amf.");
        require(backendName(EncoderBackend::QSV) == "Intel Quick Sync", "Quick Sync must have a clear UI name.");
        require(backendName(EncoderBackend::AMF) == "AMD AMF", "AMF must have a clear UI name.");

        settings.backend = EncoderBackend::NVENC;
        settings.codec = VideoCodec::H265;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
        require(containsPair(command, "-c:v", "hevc_nvenc"), "H.265 NVENC must select hevc_nvenc.");
        require(containsPair(command, "-tag:v", "hvc1"), "H.265 MP4 must use the hvc1 sample entry.");

        settings.codec = VideoCodec::AV1;
        settings.crf = 63;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
        require(containsPair(command, "-c:v", "av1_nvenc"), "AV1 NVENC must select av1_nvenc.");
        require(containsPair(command, "-cq", "63"), "NVENC AV1 must accept quality values up to 63.");
        require(containsPair(command, "-tag:v", "av01"), "AV1 MP4 must use the av01 sample entry.");

        settings.audioBitrateKbps = 320;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
        require(containsPair(command, "-b:a", "320k"), "The highest audio bitrate must be supported.");
        settings.audioBitrateKbps = 48;
        bool rejectedAudioBitrate = false;
        try {
            (void)buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
        } catch (const std::invalid_argument&) {
            rejectedAudioBitrate = true;
        }
        require(rejectedAudioBitrate, "Out-of-range audio bitrates must be rejected.");

        settings.audioBitrateKbps = 48;
        settings.audioMode = AudioMode::Copy;
        settings.outputResolution = OutputResolution::FullHD1080;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
        const std::string fullHdFilter =
            "scale=w='min(iw,if(gte(iw,ih),1920,1080))':"
            "h='min(ih,if(gte(iw,ih),1080,1920))':"
            "force_original_aspect_ratio=decrease:force_divisible_by=2";
        require(containsPair(command, "-vf", fullHdFilter),
                "Resolution presets must apply a no-upscale aspect-ratio-preserving scale filter.");
        require(containsPair(command, "-c:a", "copy"),
                "Copy audio mode must preserve the source audio stream.");
        require(!containsPair(command, "-b:a", "48k"),
                "Copy audio mode must not apply an audio re-encoding bitrate.");
        require(containsPair(command, "-map", "0:a?"),
                "Copy audio mode must map optional source audio streams.");
        require(audioModeName(AudioMode::Copy) == "保留原音频",
                "The UI must name the audio copy mode clearly.");
        require(outputResolutionName(OutputResolution::FullHD1080) == "最高 1080p",
                "The UI must name the output resolution preset clearly.");

        settings.audioMode = AudioMode::Remove;
        command = buildFfmpegCommand(settings, capabilities, "ffmpeg", "temporary.mp4");
        require(!containsPair(command, "-map", "0:a?"),
                "Remove audio mode must omit source audio streams.");
        require(!containsPair(command, "-c:a", "copy") &&
                !containsPair(command, "-c:a", "aac"),
                "Remove audio mode must not configure an audio encoder.");
        require(audioModeName(AudioMode::Remove) == "移除音频",
                "The UI must name the audio removal mode clearly.");

        settings.audioMode = AudioMode::Encode;
        settings.audioBitrateKbps = 192;
        settings.outputResolution = OutputResolution::Original;

        require(
            isVideoFile("sample.mov") && isVideoFile("clip.MP4"),
            "Common video extensions must be recognized case-insensitively.");
        require(
            isSupportedArchiveFile("media.zip") && isSupportedArchiveFile("slides.pptx"),
            "ZIP and PPTX must be recognized as batch inputs.");
        require(
            !isSupportedArchiveFile("document.docx"),
            "Non-video document archives must not be treated as supported input.");
        ConversionSettings fingerprintSettings = settings;
        fingerprintSettings.codec = VideoCodec::H264;
        fingerprintSettings.backend = EncoderBackend::CPU;
        fingerprintSettings.compression = CompressionLevel::Balanced;
        fingerprintSettings.crf = 23;
        fingerprintSettings.audioBitrateKbps = 192;
        const auto fingerprint = settingsFingerprint(fingerprintSettings);
        auto changedFingerprint = fingerprintSettings;
        changedFingerprint.audioBitrateKbps = 256;
        require(
            fingerprint != settingsFingerprint(changedFingerprint),
            "The repeat-processing marker must include audio settings.");
        changedFingerprint = fingerprintSettings;
        changedFingerprint.audioMode = AudioMode::Copy;
        require(
            fingerprint != settingsFingerprint(changedFingerprint),
            "The repeat-processing marker must include the audio handling mode.");
        auto copiedAudioFingerprint = fingerprintSettings;
        copiedAudioFingerprint.audioMode = AudioMode::Copy;
        copiedAudioFingerprint.audioBitrateKbps = 96;
        const auto copiedAudioMarker = settingsFingerprint(copiedAudioFingerprint);
        copiedAudioFingerprint.audioBitrateKbps = 320;
        require(
            copiedAudioMarker == settingsFingerprint(copiedAudioFingerprint),
            "Unused audio bitrate values must not invalidate copied audio output.");
        auto mutedAudioFingerprint = fingerprintSettings;
        mutedAudioFingerprint.audioMode = AudioMode::Remove;
        mutedAudioFingerprint.audioBitrateKbps = 96;
        const auto mutedAudioMarker = settingsFingerprint(mutedAudioFingerprint);
        mutedAudioFingerprint.audioBitrateKbps = 320;
        require(
            mutedAudioMarker == settingsFingerprint(mutedAudioFingerprint),
            "Unused audio bitrate values must not invalidate muted output.");
        changedFingerprint = fingerprintSettings;
        changedFingerprint.trimStartSeconds = 1.125;
        changedFingerprint.trimEndSeconds = 2.625;
        require(fingerprint != settingsFingerprint(changedFingerprint) &&
                outputSuffix(changedFingerprint).find("_T1125-2625") != std::string::npos,
                "Changing the trim interval must update output names and processing markers.");
        changedFingerprint.trimEndSeconds = 3.625;
        require(settingsFingerprint(changedFingerprint) !=
                settingsFingerprint(fingerprintSettings),
                "Trim end times must participate in the processing marker.");
        changedFingerprint = fingerprintSettings;
        changedFingerprint.outputResolution = OutputResolution::HD720;
        require(
            fingerprint != settingsFingerprint(changedFingerprint),
            "The repeat-processing marker must include the output resolution.");
        require(
            outputSuffix(fingerprintSettings).find("H264") != std::string::npos,
            "Output suffixes must identify the selected video codec.");
        require(
            outputSuffix(fingerprintSettings).find("A192") != std::string::npos,
            "Output suffixes must identify the selected audio bitrate.");
        auto copyAudioSuffix = fingerprintSettings;
        copyAudioSuffix.audioMode = AudioMode::Copy;
        require(outputSuffix(copyAudioSuffix).ends_with("_ACOPY"),
                "Output suffixes must identify copied audio.");
        auto mutedAudioSuffix = fingerprintSettings;
        mutedAudioSuffix.audioMode = AudioMode::Remove;
        require(outputSuffix(mutedAudioSuffix).ends_with("_ANOAUDIO"),
                "Output suffixes must identify removed audio.");
        auto scaledSuffix = fingerprintSettings;
        scaledSuffix.outputResolution = OutputResolution::FullHD1080;
        require(outputSuffix(scaledSuffix).ends_with("_A192_R1080"),
                "Output suffixes must identify the selected resolution.");
        require(outputVideoExtension(OutputContainer::MP4, "sample.mov") == ".mp4",
                "MP4 mode must use the MP4 extension for every source container.");
        require(outputVideoExtension(OutputContainer::SameAsSource, "sample.MOV") == ".mov",
                "Same-container mode must preserve the source file extension.");
        require(outputContainerName(OutputContainer::SameAsSource) == "与源文件相同",
                "The UI must name the same-container option clearly.");
        require(outputSuffix(fingerprintSettings).ends_with("_A192"),
                "The default MP4 output name must remain concise and compatible.");
        auto sameContainerSuffix = fingerprintSettings;
        sameContainerSuffix.outputContainer = OutputContainer::SameAsSource;
        require(outputSuffix(sameContainerSuffix).ends_with("_A192_SRC"),
                "Same-container output names must identify the source-container mode.");
        auto sameContainer = fingerprintSettings;
        sameContainer.outputContainer = OutputContainer::SameAsSource;
        require(settingsFingerprint(sameContainer) != settingsFingerprint(fingerprintSettings),
                "The repeat-processing marker must include the selected output container.");
        command = buildFfmpegCommand(sameContainer, capabilities, "ffmpeg", "temporary.mkv");
        require(!containsPair(command, "-movflags", "+faststart"),
                "Matroska output must not receive MP4-only muxer options.");
        require(!containsPair(command, "-tag:v", "hvc1"),
                "Matroska output must not receive the MP4 H.265 sample-entry tag.");
        sameContainer.codec = VideoCodec::H265;
        command = buildFfmpegCommand(sameContainer, capabilities, "ffmpeg", "temporary.mp4");
        require(containsPair(command, "-tag:v", "hvc1"),
                "H.265 MP4 output must retain the hvc1 sample-entry tag.");
        const auto automaticPlan = recommendResourcePlan(12, EncoderBackend::CPU, VideoCodec::H264, 0);
        require(automaticPlan.parallelJobs >= 1 && automaticPlan.threadsPerJob >= 1,
                "Automatic scheduling must always allocate at least one worker and one thread.");
        require(automaticPlan.parallelJobs * automaticPlan.threadsPerJob >= automaticPlan.logicalCores,
                "Automatic CPU scheduling must allocate enough encoding threads to use all logical cores.");
        const auto limitedPlan = recommendResourcePlan(12, EncoderBackend::CPU, VideoCodec::H264, 2);
        require(limitedPlan.parallelJobs <= 2, "A manual parallel-task limit must be respected.");
        const auto hardwarePlan = recommendResourcePlan(12, EncoderBackend::NVENC, VideoCodec::H264, 0);
        require(hardwarePlan.parallelJobs <= 2, "Hardware encoders must use a bounded concurrent session count.");
        require(!EncoderCapabilities{}.supports(VideoCodec::H264, EncoderBackend::QSV),
                "Unavailable hardware encoders must remain hidden from the interface.");

        require(crfMaximum(VideoCodec::H264) == 51, "H.264 CRF range must end at 51.");
        require(crfMaximum(VideoCodec::H265) == 51, "H.265 CRF range must end at 51.");
        require(crfMaximum(VideoCodec::AV1) == 63, "AV1 CRF range must end at 63.");

        {
            namespace fs = std::filesystem;
            const fs::path testDirectory = fs::temp_directory_path() / "ffmpeg-studio-ssh-test";
            std::error_code cleanupError;
            fs::remove_all(testDirectory, cleanupError);
            fs::create_directories(testDirectory);
            const fs::path configPath = testDirectory / "config";
#ifdef _WIN32
            const auto oldConfig = environmentVariable("FFMPEG_STUDIO_SSH_CONFIG");
            const std::string oldConfigValue = oldConfig.value_or(std::string{});
            _putenv_s("FFMPEG_STUDIO_SSH_CONFIG", configPath.string().c_str());
#else
            const auto oldConfig = environmentVariable("FFMPEG_STUDIO_SSH_CONFIG");
            const std::string oldConfigValue = oldConfig.value_or(std::string{});
            setenv("FFMPEG_STUDIO_SSH_CONFIG", configPath.c_str(), 1);
#endif
            {
                std::ofstream config(configPath, std::ios::binary);
                config << "Host *\n  ServerAliveInterval 30\n\n"
                       << "Host shared-node backup-node\n  HostName 10.0.0.5\n  User admin\n"
                       << "  Port 2222\n  Compression yes\n  IdentityFile ~/.ssh/id_ed25519\n\n"
                       << "Host other-node\n  HostName example.invalid\n"
                       << "Match host *.example\n  User match-user\n";
            }
            SshService ssh;
            auto servers = ssh.listServers();
            require(
                servers.size() == 3,
                "SSH config discovery must list concrete aliases and ignore wildcard patterns.");
            require(SshService::validAlias("media-node_1.example") && !SshService::validAlias("-invalid"),
                    "SSH aliases must reject option-like names.");
            require(SshService::shellQuote("a'b") == "'a'\\''b'",
                    "Remote shell arguments must be quoted safely.");

            SshServer keyServer;
            keyServer.alias = "shared-node";
            keyServer.hostName = "media.internal";
            keyServer.user = "worker";
            keyServer.port = 2200;
            keyServer.identityFile = "C:/Keys/worker key_ed25519";
            keyServer.authMethod = SshAuthMethod::PublicKey;
            std::string sshError;
            require(ssh.saveServer(keyServer, {}, sshError), "Saving an SSH server must update the local SSH config.");
            servers = ssh.listServers();
            const auto keyEntry = std::find_if(
                servers.begin(), servers.end(),
                [](const SshServer& server) {
                    return server.alias == "shared-node";
                });
            require(keyEntry != servers.end() && keyEntry->hostName == "media.internal" &&
                    keyEntry->user == "worker" && keyEntry->port == 2200 &&
                    keyEntry->identityFile == "C:/Keys/worker key_ed25519",
                    "The SSH manager must write and read server connection settings.");
            const auto otherEntry = std::find_if(
                servers.begin(), servers.end(),
                [](const SshServer& server) {
                    return server.alias == "other-node";
                });
            require(otherEntry != servers.end() && otherEntry->user.empty(),
                    "Match blocks must not be interpreted as Host options.");
            const auto backupEntry = std::find_if(
                servers.begin(), servers.end(),
                [](const SshServer& server) {
                    return server.alias == "backup-node";
                });
            require(backupEntry != servers.end() && backupEntry->hostName == "10.0.0.5" &&
                    backupEntry->user == "admin",
                    "Editing one alias from a shared Host block must preserve the other aliases.");
            std::string updatedText;
            {
                std::ifstream updatedConfig(configPath, std::ios::binary);
                updatedText.assign(
                    std::istreambuf_iterator<char>(updatedConfig),
                    std::istreambuf_iterator<char>());
            }
            require(updatedText.find("Compression yes") != std::string::npos &&
                    updatedText.find("ServerAliveInterval 30") != std::string::npos,
                    "Updating a server must preserve unrelated SSH options.");

            SshServer passwordServer;
            passwordServer.alias = "password-node";
            passwordServer.hostName = "password.internal";
            passwordServer.user = "encoder";
            passwordServer.authMethod = SshAuthMethod::Password;
            const bool passwordSaved =
                ssh.saveServer(passwordServer, "test-password", sshError);
            require(
                passwordSaved,
                "Password authentication must save the secret through the platform credential store: " +
                    sshError);
            require(ssh.hasPassword("password-node"),
                    "A saved SSH password must be available to the askpass helper.");
#ifdef _WIN32
            const auto askpassSecret = ssh.passwordForAskpass("password-node");
#else
            setenv("FFMPEG_STUDIO_SSH_PASSWORD", "askpass-secret", 1);
            const auto askpassSecret = ssh.passwordForAskpass("password-node");
            unsetenv("FFMPEG_STUDIO_SSH_PASSWORD");
#endif
            require(askpassSecret.has_value() && !askpassSecret->empty(),
                    "The askpass helper must retrieve a password without placing it in process arguments.");
            require(updatedText.find("test-password") == std::string::npos,
                    "SSH passwords must never be written to the SSH configuration file.");
            require(ssh.removeServer("password-node", sshError),
                    "Removing a managed SSH server must update the config.");
            require(!ssh.hasPassword("password-node"),
                    "Removing a server must remove its saved password.");
            require(ssh.removeServer("shared-node", sshError), "Removing one SSH alias must succeed.");
            servers = ssh.listServers();
            require(
                std::none_of(
                    servers.begin(), servers.end(),
                    [](const SshServer& server) {
                        return server.alias == "shared-node";
                    }) &&
                std::any_of(
                    servers.begin(), servers.end(),
                    [](const SshServer& server) {
                        return server.alias == "backup-node";
                    }),
                "Removing one SSH alias must leave unrelated aliases intact.");
            require(ssh.removeServer("other-node", sshError),
                    "Removing a Host block before a Match block must succeed.");
            std::string finalText;
            {
                std::ifstream finalConfig(configPath, std::ios::binary);
                finalText.assign(
                    std::istreambuf_iterator<char>(finalConfig),
                    std::istreambuf_iterator<char>());
            }
            require(finalText.find("Match host *.example") != std::string::npos &&
                    finalText.find("User match-user") != std::string::npos,
                    "Removing a Host block must preserve following Match sections.");

#ifdef _WIN32
            _putenv_s("FFMPEG_STUDIO_SSH_CONFIG", oldConfigValue.c_str());
#else
            if (oldConfigValue.empty()) unsetenv("FFMPEG_STUDIO_SSH_CONFIG");
            else setenv("FFMPEG_STUDIO_SSH_CONFIG", oldConfigValue.c_str(), 1);
#endif
            fs::remove_all(testDirectory, cleanupError);
        }

        {
            namespace fs = std::filesystem;
            const fs::path testDirectory = fs::temp_directory_path() /
                ("video-compression-workbench-queue-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::error_code cleanupError;
            fs::remove_all(testDirectory, cleanupError);
            fs::create_directories(testDirectory);
            const fs::path inputFile = testDirectory / "resume-after-restart.mp4";
            {
                std::ofstream output(inputFile, std::ios::binary);
                output << "queue recovery fixture";
            }
            const auto previousConfigDirectory =
                environmentVariable("FFMPEG_STUDIO_CONFIG_DIR");
#ifdef _WIN32
            _putenv_s("FFMPEG_STUDIO_CONFIG_DIR", pathToUtf8(testDirectory / "config").c_str());
#else
            setenv("FFMPEG_STUDIO_CONFIG_DIR",
                   pathToUtf8(testDirectory / "config").c_str(), 1);
#endif
            {
                AppController firstRun;
                auto settings = firstRun.snapshot().settings;
                settings.audioMode = AudioMode::Copy;
                settings.outputResolution = OutputResolution::HD720;
                settings.trimStartSeconds = 0.125;
                settings.trimEndSeconds = 1.500;
                firstRun.updateSettings(settings);
                firstRun.addInputs({pathToUtf8(inputFile)});
                waitForScan(firstRun, 1);
                const auto added = firstRun.snapshot();
                require(firstRun.setVideoTrim(added.tasks.front().id, "", {0.125, 1.5}),
                        "Individual video trim must be editable.");
                require(added.tasks.front().items.front().trim.startSeconds == 0.0,
                        "Unedited videos must keep their original full duration.");
                require(firstRun.snapshot().tasks.front().items.front().trim.startSeconds == 0.125,
                        "Editing one video must update the selected queue item.");
            }
            {
                AppController restoredRun;
                waitForScan(restoredRun, 1);
                const auto restored = restoredRun.snapshot();
                require(restored.totalTasks == 1,
                        "The pending task queue must be restored after restarting the application.");
                require(restored.settings.audioMode == AudioMode::Copy &&
                        restored.settings.outputResolution == OutputResolution::HD720 &&
                        restored.settings.trimStartSeconds == 0.125 &&
                        restored.settings.trimEndSeconds == 1.5,
                        "Audio, resolution and trim settings must persist across restarts.");
                require(restored.tasks.front().items.front().trim.startSeconds == 0.125 &&
                        restored.tasks.front().items.front().trim.endSeconds == 1.5,
                        "Per-video trim intervals must persist across application restarts.");
                restoredRun.clearAll();
                require(restoredRun.snapshot().totalTasks == 0,
                        "Clearing the task list must update the persisted queue.");
            }
            {
                AppController cleanRun;
                waitForScan(cleanRun, 0);
                require(cleanRun.snapshot().totalTasks == 0,
                        "A cleared task queue must remain empty after restart.");
            }
#ifdef _WIN32
            _putenv_s("FFMPEG_STUDIO_CONFIG_DIR",
                      previousConfigDirectory ? previousConfigDirectory->c_str() : "");
#else
            if (previousConfigDirectory) {
                setenv("FFMPEG_STUDIO_CONFIG_DIR", previousConfigDirectory->c_str(), 1);
            } else {
                unsetenv("FFMPEG_STUDIO_CONFIG_DIR");
            }
#endif
            fs::remove_all(testDirectory, cleanupError);
        }

#ifndef _WIN32
        std::atomic_bool cancelProcess{false};
        std::thread cancelThread([&cancelProcess]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            cancelProcess.store(true);
        });
        const auto cancelStart = std::chrono::steady_clock::now();
        const auto processResult = runProcess({"/usr/bin/sleep", "5"}, &cancelProcess);
        cancelThread.join();
        const auto cancelElapsed = std::chrono::steady_clock::now() - cancelStart;
        require(processResult.cancelled, "The process runner must report cancellation.");
        require(cancelElapsed < std::chrono::seconds(2), "The process runner must stop a cancelled process promptly.");
#endif

        std::cout << "All core tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Test failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
