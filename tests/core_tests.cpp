#include "core/conversion.hpp"
#include "core/process.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool containsPair(const std::vector<std::string>& command,
                  const std::string& option, const std::string& value) {
    for (std::size_t index = 0; index + 1 < command.size(); ++index) {
        if (command[index] == option && command[index + 1] == value) return true;
    }
    return false;
}

} // namespace

int main() {
    try {
        using namespace ffmpegui;
        EncoderCapabilities capabilities;
        capabilities.libx264 = true;
        capabilities.libx265 = true;
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
        const std::vector<std::string> expectedX264{
            "ultrafast", "veryfast", "fast", "medium", "slow", "slower", "veryslow", "placebo"};
        const std::vector<std::string> expectedSvt{"13", "11", "10", "8", "6", "4", "2", "0"};
        const std::vector<std::string> expectedAom{"8", "7", "6", "4", "3", "2", "1", "0"};
        const std::vector<std::string> expectedNvenc{"p1", "p2", "p3", "p4", "p5", "p6", "p7", "p7"};
        const std::vector<std::string> expectedQsv{"veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", "veryslow"};

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
        require(containsPair(command, "-threads:v", "0") == false, "An unset thread limit must not add an invalid thread option.");
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
        require(rejectedWebmH264, "WebM must reject H.264 when only H.264/H.265/AV1 choices are available.");
        require(std::any_of(command.begin(), command.end(), [](const std::string& arg) { return arg.find("FFmpegStudio:v2:") != std::string::npos; }),
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
            require(containsPair(command, "-global_quality", "23"), "Quick Sync must receive the selected quality value.");
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

        require(isVideoFile("sample.mov") && isVideoFile("clip.MP4"), "Common video extensions must be recognized case-insensitively.");
        require(isSupportedArchiveFile("media.zip") && isSupportedArchiveFile("slides.pptx"), "ZIP and PPTX must be recognized as batch inputs.");
        require(!isSupportedArchiveFile("document.docx"), "Non-video document archives must not be treated as supported input.");
        ConversionSettings fingerprintSettings = settings;
        fingerprintSettings.codec = VideoCodec::H264;
        fingerprintSettings.backend = EncoderBackend::CPU;
        fingerprintSettings.compression = CompressionLevel::Balanced;
        fingerprintSettings.crf = 23;
        fingerprintSettings.audioBitrateKbps = 192;
        const auto fingerprint = settingsFingerprint(fingerprintSettings);
        auto changedFingerprint = fingerprintSettings;
        changedFingerprint.audioBitrateKbps = 256;
        require(fingerprint != settingsFingerprint(changedFingerprint), "The repeat-processing marker must include audio settings.");
        require(outputSuffix(fingerprintSettings).find("H264") != std::string::npos, "Output suffixes must identify the selected video codec.");
        require(outputSuffix(fingerprintSettings).find("A192") != std::string::npos, "Output suffixes must identify the selected audio bitrate.");
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
            const char* oldConfig = std::getenv("FFMPEG_STUDIO_SSH_CONFIG");
            const std::string oldConfigValue = oldConfig == nullptr ? std::string{} : oldConfig;
            _putenv_s("FFMPEG_STUDIO_SSH_CONFIG", configPath.string().c_str());
#else
            const char* oldConfig = std::getenv("FFMPEG_STUDIO_SSH_CONFIG");
            const std::string oldConfigValue = oldConfig == nullptr ? std::string{} : oldConfig;
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
            require(servers.size() == 3, "SSH config discovery must list concrete aliases and ignore wildcard patterns.");
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
            const auto keyEntry = std::find_if(servers.begin(), servers.end(), [](const auto& item) { return item.alias == "shared-node"; });
            require(keyEntry != servers.end() && keyEntry->hostName == "media.internal" &&
                    keyEntry->user == "worker" && keyEntry->port == 2200 &&
                    keyEntry->identityFile == "C:/Keys/worker key_ed25519",
                    "The SSH manager must write and read server connection settings.");
            const auto otherEntry = std::find_if(servers.begin(), servers.end(), [](const auto& item) { return item.alias == "other-node"; });
            require(otherEntry != servers.end() && otherEntry->user.empty(),
                    "Match blocks must not be interpreted as Host options.");
            const auto backupEntry = std::find_if(servers.begin(), servers.end(), [](const auto& item) { return item.alias == "backup-node"; });
            require(backupEntry != servers.end() && backupEntry->hostName == "10.0.0.5" &&
                    backupEntry->user == "admin",
                    "Editing one alias from a shared Host block must preserve the other aliases.");
            std::ifstream updatedConfig(configPath, std::ios::binary);
            const std::string updatedText((std::istreambuf_iterator<char>(updatedConfig)), {});
            require(updatedText.find("Compression yes") != std::string::npos &&
                    updatedText.find("ServerAliveInterval 30") != std::string::npos,
                    "Updating a server must preserve unrelated SSH options.");

            SshServer passwordServer;
            passwordServer.alias = "password-node";
            passwordServer.hostName = "password.internal";
            passwordServer.user = "encoder";
            passwordServer.authMethod = SshAuthMethod::Password;
            require(ssh.saveServer(passwordServer, "test-password", sshError),
                    "Password authentication must save the secret through the platform credential store.");
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
            require(std::none_of(servers.begin(), servers.end(), [](const auto& item) { return item.alias == "shared-node"; }) &&
                    std::any_of(servers.begin(), servers.end(), [](const auto& item) { return item.alias == "backup-node"; }),
                    "Removing one SSH alias must leave unrelated aliases intact.");
            require(ssh.removeServer("other-node", sshError),
                    "Removing a Host block before a Match block must succeed.");
            std::ifstream finalConfig(configPath, std::ios::binary);
            const std::string finalText((std::istreambuf_iterator<char>(finalConfig)), {});
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
