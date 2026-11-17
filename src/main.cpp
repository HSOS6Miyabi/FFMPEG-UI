#include "core/app_controller.hpp"
#include "core/archive.hpp"
#include "core/process.hpp"
#include "core/ssh.hpp"
#include "platform/ui.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;

std::string pathToUtf8(const fs::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

fs::path pathFromUtf8(std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char c : value) encoded.push_back(static_cast<char8_t>(c));
    return fs::path(encoded);
}

ffmpegui::FfmpegService makeService() {
    std::string ffmpeg = "ffmpeg", ffprobe = "ffprobe";
    if (const auto value = ffmpegui::environmentVariable("FFMPEG_PATH"); value && !value->empty()) {
        ffmpeg = *value;
    }
    if (const auto value = ffmpegui::environmentVariable("FFPROBE_PATH"); value && !value->empty()) {
        ffprobe = *value;
    }
    return ffmpegui::FfmpegService(ffmpeg, ffprobe);
}

void printUsage() {
    std::cout << "视频压缩工作台\n"
              << "Usage: video-compression-workbench [options] [files or folders...]\n\n"
              << "Options:\n"
              << "  --help, -h       Show this help\n"
              << "  --encoders       List available CPU and hardware encoders\n"
              << "  --demo           Run conversion and archive demos; keep generated files\n"
              << "  --self-test      Run local conversion, ZIP and PPTX tests\n"
              << "  --output-dir DIR Set the output directory before opening the UI\n\n"
              << "Pass one or more video files, ZIP archives, PPTX files or folders to open\n"
              << "the application with those inputs already added.\n";
}

int listEncoders(const ffmpegui::EncoderCapabilities& capabilities) {
    std::cout << capabilities.ffmpegVersion << '\n';
    const std::array<ffmpegui::VideoCodec, 3> codecs{
        ffmpegui::VideoCodec::H264, ffmpegui::VideoCodec::H265, ffmpegui::VideoCodec::AV1};
    const std::array<ffmpegui::EncoderBackend, 4> backends{
        ffmpegui::EncoderBackend::CPU, ffmpegui::EncoderBackend::NVENC,
        ffmpegui::EncoderBackend::QSV, ffmpegui::EncoderBackend::AMF};
    for (const auto codec : codecs) {
        std::cout << ffmpegui::codecName(codec) << ": ";
        bool first = true;
        for (const auto backend : backends) {
            if (!capabilities.supports(codec, backend)) continue;
            if (!first) std::cout << ", ";
            std::cout << ffmpegui::backendName(backend);
            first = false;
        }
        if (first) std::cout << "不可用";
        std::cout << '\n';
    }
    return capabilities.ffprobeAvailable ? 0 : 1;
}

bool createZip(const fs::path& archivePath, const std::vector<std::pair<std::string, fs::path>>& files) {
    archive* writer = archive_write_new();
    if (!writer) return false;
    archive_write_set_format_zip(writer);
    archive_write_set_format_option(writer, "zip", "compression", "deflate");
    const std::string archiveName = pathToUtf8(archivePath);
    if (archive_write_open_filename(writer, archiveName.c_str()) != ARCHIVE_OK) {
        archive_write_free(writer);
        return false;
    }
    bool success = true;
    for (const auto& [name, source] : files) {
        std::error_code error;
        const auto size = fs::file_size(source, error);
        if (error) { success = false; break; }
        archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, name.c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, static_cast<la_int64_t>(size));
        if (archive_write_header(writer, entry) != ARCHIVE_OK) success = false;
        if (success) {
            std::ifstream input(source, std::ios::binary);
            std::array<char, 65536> buffer{};
            while (input) {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = input.gcount();
                if (count > 0 && archive_write_data(writer, buffer.data(), static_cast<std::size_t>(count)) < 0) {
                    success = false;
                    break;
                }
            }
            if (!input.eof() && input.fail()) success = false;
        }
        archive_entry_free(entry);
        if (!success) break;
    }
    if (archive_write_close(writer) != ARCHIVE_OK) success = false;
    archive_write_free(writer);
    return success;
}

std::optional<std::string> readZipEntry(const fs::path& archivePath, const std::string& wanted) {
    archive* reader = archive_read_new();
    if (!reader) return std::nullopt;
    archive_read_support_filter_all(reader);
    archive_read_support_format_zip(reader);
    const std::string source = pathToUtf8(archivePath);
    if (archive_read_open_filename(reader, source.c_str(), 10240) != ARCHIVE_OK) {
        archive_read_free(reader);
        return std::nullopt;
    }
    archive_entry* entry = nullptr;
    std::optional<std::string> result;
    while (archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
        const char* name = archive_entry_pathname_utf8(entry);
        if (!name) name = archive_entry_pathname(entry);
        if (name && wanted == name) {
            std::string value;
            std::array<char, 4096> buffer{};
            la_ssize_t count = 0;
            while ((count = archive_read_data(reader, buffer.data(), buffer.size())) > 0)
                value.append(buffer.data(), static_cast<std::size_t>(count));
            if (count >= 0) result = std::move(value);
            break;
        }
        archive_read_data_skip(reader);
    }
    archive_read_close(reader);
    archive_read_free(reader);
    return result;
}

int runSelfTest(bool keepFiles) {
    auto service = makeService();
    const auto capabilities = service.probeCapabilities(true);
    if (capabilities.ffmpegVersion.empty() || !capabilities.ffprobeAvailable) {
        std::cerr << "FFmpeg and ffprobe must be installed before running the self-test.\n";
        return 1;
    }
    const fs::path work = fs::temp_directory_path() / ("ffmpeg-studio-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(work);
#ifdef _WIN32
    _putenv_s("FFMPEG_STUDIO_CONFIG_DIR", pathToUtf8(work / "config").c_str());
#else
    setenv("FFMPEG_STUDIO_CONFIG_DIR", pathToUtf8(work / "config").c_str(), 1);
#endif
    const fs::path source = work / "source.mp4";
    const auto generated = ffmpegui::runProcess({service.ffmpegPath(), "-hide_banner", "-loglevel", "error",
        "-y", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=24:duration=1.5",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1.5",
        "-map", "0:v:0", "-map", "1:a:0", "-c:v", "mpeg4", "-q:v", "5",
        "-c:a", "aac", "-shortest", pathToUtf8(source)});
    if (generated.exitCode != 0) {
        std::cerr << "Unable to create test media:\n" << generated.output << '\n';
        if (!keepFiles) { std::error_code ignored; fs::remove_all(work, ignored); }
        return 1;
    }

    int passed = 0, failed = 0;
    auto check = [&](const std::string& name, bool ok) {
        std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
        ok ? ++passed : ++failed;
    };
    const std::array<ffmpegui::VideoCodec, 3> codecs{
        ffmpegui::VideoCodec::H264, ffmpegui::VideoCodec::H265, ffmpegui::VideoCodec::AV1};
    if (capabilities.supports(ffmpegui::VideoCodec::H264, ffmpegui::EncoderBackend::CPU)) {
        const fs::path resolutionSource = work / "resolution-source.mp4";
        const auto resolutionSourceGenerated = ffmpegui::runProcess({
            service.ffmpegPath(), "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=24:duration=1",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1",
            "-map", "0:v:0", "-map", "1:a:0",
            "-c:v", "mpeg4", "-q:v", "5", "-c:a", "aac", "-shortest",
            pathToUtf8(resolutionSource)
        });
        ffmpegui::ConversionSettings scaledSettings;
        scaledSettings.inputPath = pathToUtf8(resolutionSource);
        scaledSettings.outputPath = pathToUtf8(work / "scaled-480p.mp4");
        scaledSettings.codec = ffmpegui::VideoCodec::H264;
        scaledSettings.backend = ffmpegui::EncoderBackend::CPU;
        scaledSettings.compression = ffmpegui::CompressionLevel::Fastest;
        scaledSettings.crf = 25;
        scaledSettings.audioBitrateKbps = 192;
        scaledSettings.audioMode = ffmpegui::AudioMode::Copy;
        scaledSettings.outputResolution = ffmpegui::OutputResolution::SD480;
        const auto scaledResult = resolutionSourceGenerated.exitCode == 0
            ? service.convert(scaledSettings, capabilities, {}, nullptr)
            : ffmpegui::ConversionResult{};
        const auto scaledProbe = scaledResult.success
            ? ffmpegui::runProcess({
                service.ffprobePath(), "-v", "error",
                "-show_entries", "stream=codec_type,width,height,codec_name",
                "-of", "default=noprint_wrappers=1", scaledResult.outputPath
            })
            : ffmpegui::ProcessResult{};
        check("480p output resolution and original audio copy",
            scaledResult.success &&
            scaledProbe.output.find("width=854") != std::string::npos &&
            scaledProbe.output.find("height=480") != std::string::npos &&
            scaledProbe.output.find("codec_name=aac") != std::string::npos);

        auto mutedSettings = scaledSettings;
        mutedSettings.outputPath = pathToUtf8(work / "without-audio.mp4");
        mutedSettings.audioMode = ffmpegui::AudioMode::Remove;
        mutedSettings.outputResolution = ffmpegui::OutputResolution::Original;
        const auto mutedResult = resolutionSourceGenerated.exitCode == 0
            ? service.convert(mutedSettings, capabilities, {}, nullptr)
            : ffmpegui::ConversionResult{};
        const auto mutedProbe = mutedResult.success
            ? ffmpegui::runProcess({
                service.ffprobePath(), "-v", "error",
                "-show_entries", "stream=codec_type",
                "-of", "default=noprint_wrappers=1", mutedResult.outputPath
            })
            : ffmpegui::ProcessResult{};
        check("audio removal",
            mutedResult.success &&
            mutedProbe.output.find("codec_type=audio") == std::string::npos);

        const fs::path sameContainerSource = work / "source.mkv";
        const fs::path sameContainerOutput = work / "same-container.mkv";
        const auto remux = ffmpegui::runProcess({service.ffmpegPath(), "-hide_banner", "-loglevel", "error",
            "-y", "-i", pathToUtf8(source), "-map", "0", "-c", "copy", pathToUtf8(sameContainerSource)});
        ffmpegui::ConversionSettings sameContainerSettings;
        sameContainerSettings.inputPath = pathToUtf8(sameContainerSource);
        sameContainerSettings.outputPath = pathToUtf8(sameContainerOutput);
        sameContainerSettings.outputContainer = ffmpegui::OutputContainer::SameAsSource;
        sameContainerSettings.codec = ffmpegui::VideoCodec::H264;
        sameContainerSettings.backend = ffmpegui::EncoderBackend::CPU;
        sameContainerSettings.compression = ffmpegui::CompressionLevel::Fastest;
        sameContainerSettings.crf = 25;
        sameContainerSettings.audioBitrateKbps = 128;
        const auto sameContainerResult = remux.exitCode == 0
            ? service.convert(sameContainerSettings, capabilities, {}, nullptr)
            : ffmpegui::ConversionResult{};
        const auto sameContainerProbe = sameContainerResult.success
            ? ffmpegui::runProcess({service.ffprobePath(), "-v", "error", "-show_entries", "format=format_name",
                "-of", "default=noprint_wrappers=1:nokey=1", pathToUtf8(sameContainerOutput)})
            : ffmpegui::ProcessResult{};
        check(
            "same-container video conversion",
            sameContainerResult.success &&
                sameContainerOutput.extension() == ".mkv" &&
                sameContainerProbe.output.find("matroska") != std::string::npos);

        if (capabilities.libsvtav1 && capabilities.libopus) {
            const fs::path webmSource = work / "source.webm";
            const fs::path webmOutput = work / "same-container.webm";
            const auto webmGenerated = ffmpegui::runProcess({service.ffmpegPath(), "-hide_banner", "-loglevel", "error",
                "-y", "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=12:duration=1",
                "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1",
                "-c:v", "libvpx-vp9", "-deadline", "realtime", "-cpu-used", "8",
                "-c:a", "libopus", "-b:a", "96k", pathToUtf8(webmSource)});
            ffmpegui::ConversionSettings webmSettings;
            webmSettings.inputPath = pathToUtf8(webmSource);
            webmSettings.outputPath = pathToUtf8(webmOutput);
            webmSettings.outputContainer = ffmpegui::OutputContainer::SameAsSource;
            webmSettings.codec = ffmpegui::VideoCodec::AV1;
            webmSettings.backend = ffmpegui::EncoderBackend::CPU;
            webmSettings.compression = ffmpegui::CompressionLevel::Fastest;
            webmSettings.crf = 45;
            webmSettings.audioBitrateKbps = 96;
            const auto webmResult = webmGenerated.exitCode == 0
                ? service.convert(webmSettings, capabilities, {}, nullptr)
                : ffmpegui::ConversionResult{};
            const auto webmProbe = webmResult.success
                ? ffmpegui::runProcess({service.ffprobePath(), "-v", "error", "-show_entries",
                    "format=format_name:stream=codec_type,codec_name", "-of", "default=noprint_wrappers=1",
                    pathToUtf8(webmOutput)})
                : ffmpegui::ProcessResult{};
            check("same-container WebM with AV1 and Opus", webmResult.success &&
                webmProbe.output.find("webm") != std::string::npos &&
                webmProbe.output.find("codec_name=av1") != std::string::npos &&
                webmProbe.output.find("codec_name=opus") != std::string::npos);
        }
    }
    for (const auto codec : codecs) {
        if (!capabilities.supports(codec, ffmpegui::EncoderBackend::CPU)) {
            std::cout << "SKIP " << ffmpegui::codecName(codec) << " CPU encoder is unavailable\n";
            continue;
        }
        ffmpegui::ConversionSettings settings;
        settings.inputPath = pathToUtf8(source);
        settings.outputPath = pathToUtf8(work / (codec == ffmpegui::VideoCodec::H264 ? "encoded-h264.mp4" :
            codec == ffmpegui::VideoCodec::H265 ? "encoded-h265.mp4" : "encoded-av1.mp4"));
        settings.codec = codec;
        settings.backend = ffmpegui::EncoderBackend::CPU;
        settings.compression = ffmpegui::CompressionLevel::Fastest;
        settings.crf = ffmpegui::defaultCrf(codec);
        settings.audioBitrateKbps = 160;
        settings.threadsPerJob = 2;
        bool frame=false,time=false,duration=false,fps=false,speed=false,bitrate=false,size=false;
        const auto result = service.convert(settings, capabilities, [&](const auto& p) {
            frame |= p.frame.has_value(); time |= p.currentTimeSeconds.has_value();
            duration |= p.durationSeconds.has_value(); fps |= p.fps.has_value();
            speed |= p.speed.has_value(); bitrate |= p.bitrate.has_value(); size |= p.totalSizeBytes.has_value();
        }, nullptr);
        check(ffmpegui::codecName(codec) + " CPU conversion", result.success);
        if (result.success) {
            check(
                ffmpegui::codecName(codec) + " structured progress",
                frame && time && duration && fps && speed && bitrate && size);
            const auto audio = ffmpegui::runProcess({service.ffprobePath(), "-v", "error", "-select_streams", "a:0",
                "-show_entries", "stream=codec_name", "-of", "default=noprint_wrappers=1:nokey=1", result.outputPath});
            check(
                ffmpegui::codecName(codec) + " AAC audio",
                audio.exitCode == 0 &&
                    audio.output.find("aac") != std::string::npos);
            check(
                ffmpegui::codecName(codec) + " embedded parameter marker",
                service.isAlreadyProcessed(result.outputPath, settings));
            check(ffmpegui::codecName(codec) + " source-specific output marker",
                service.isOutputForSourceAlreadyProcessed(result.outputPath, settings));
            auto repeated = settings;
            repeated.inputPath = result.outputPath;
            repeated.outputPath = pathToUtf8(work / ("must-be-skipped-" + ffmpegui::codecName(codec) + ".mp4"));
            const auto skip = service.convert(repeated, capabilities, {}, nullptr);
            check(ffmpegui::codecName(codec) + " duplicate processing skip", skip.success && skip.skipped &&
                fs::is_regular_file(pathFromUtf8(repeated.outputPath)) &&
                fs::file_size(pathFromUtf8(repeated.outputPath)) == fs::file_size(pathFromUtf8(result.outputPath)));
        }
    }

#ifndef _WIN32
    {
        const auto originalSshPath = ffmpegui::environmentVariable("FFMPEG_STUDIO_SSH_PATH");
        const auto originalScpPath = ffmpegui::environmentVariable("FFMPEG_STUDIO_SCP_PATH");
        const auto originalConfigPath = ffmpegui::environmentVariable("FFMPEG_STUDIO_SSH_CONFIG");
        const bool useRealSsh =
            ffmpegui::environmentVariable("FFMPEG_STUDIO_SSH_TEST_REAL").value_or("") == "1";
        const auto realSshUser =
            ffmpegui::environmentVariable("FFMPEG_STUDIO_SSH_TEST_USER");
        const auto realSshKey =
            ffmpegui::environmentVariable("FFMPEG_STUDIO_SSH_TEST_KEY");
        const auto realSshPort =
            ffmpegui::environmentVariable("FFMPEG_STUDIO_SSH_TEST_PORT");
        const auto realSshPassword =
            ffmpegui::environmentVariable("FFMPEG_STUDIO_SSH_TEST_PASSWORD");
        unsigned short realPort = 22;
        bool realSshReady = false;
        if (useRealSsh && realSshUser && !realSshUser->empty() &&
            realSshKey && fs::is_regular_file(pathFromUtf8(*realSshKey)) &&
            realSshPort && realSshPassword && !realSshPassword->empty()) {
            try {
                const unsigned long parsedPort = std::stoul(*realSshPort);
                if (parsedPort >= 1 && parsedPort <= 65535) {
                    realPort = static_cast<unsigned short>(parsedPort);
                    realSshReady = true;
                }
            } catch (const std::exception&) {
                realSshReady = false;
            }
        }

        const fs::path mockSsh = work / "mock-ssh";
        const fs::path mockScp = work / "mock-scp";
        const fs::path sshConfig = work / "ssh-config";

        {
            std::ofstream script(mockSsh, std::ios::binary);
            script << "#!/bin/sh\n"
                   << "set -eu\n"
                   << "while [ \"$#\" -gt 0 ]; do\n"
                   << "  case \"$1\" in\n"
                   << "    -T) shift ;;\n"
                   << "    -o|-F) shift 2 ;;\n"
                   << "    *) break ;;\n"
                   << "  esac\n"
                   << "done\n"
                   << "[ \"$#\" -eq 2 ] || exit 64\n"
                   << "remote_command=\"$2\"\n"
                   << "exec /bin/sh -c \"$remote_command\"\n";
        }

        {
            std::ofstream script(mockScp, std::ios::binary);
            script << "#!/bin/sh\n"
                   << "set -eu\n"
                   << "while [ \"$#\" -gt 0 ]; do\n"
                   << "  case \"$1\" in\n"
                   << "    -q) shift ;;\n"
                   << "    -o|-F) shift 2 ;;\n"
                   << "    *) break ;;\n"
                   << "  esac\n"
                   << "done\n"
                   << "[ \"$#\" -eq 2 ] || exit 64\n"
                   << "source_path=\"$1\"\n"
                   << "destination_path=\"$2\"\n"
                   << "case \"$source_path\" in\n"
                   << "  *:*) remote_path=$(printf '%s\\n' \"$source_path\" | sed 's/^[^:]*://'); "
                      "cp -- \"$remote_path\" \"$destination_path\" ;;\n"
                   << "  *) case \"$destination_path\" in\n"
                   << "       *:*) remote_path=$(printf '%s\\n' \"$destination_path\" | sed 's/^[^:]*://'); "
                      "cp -- \"$source_path\" \"$remote_path\" ;;\n"
                   << "       *) exit 64 ;;\n"
                   << "     esac ;;\n"
                   << "esac\n";
        }

        const bool scriptsWritten =
            fs::is_regular_file(mockSsh) && fs::is_regular_file(mockScp);
        const bool scriptsExecutable =
            scriptsWritten &&
            ::chmod(mockSsh.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) == 0 &&
            ::chmod(mockScp.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) == 0;

        if (useRealSsh) {
            setenv("FFMPEG_STUDIO_SSH_PATH", "ssh", 1);
            setenv("FFMPEG_STUDIO_SCP_PATH", "scp", 1);
        } else {
            setenv("FFMPEG_STUDIO_SSH_PATH", pathToUtf8(mockSsh).c_str(), 1);
            setenv("FFMPEG_STUDIO_SCP_PATH", pathToUtf8(mockScp).c_str(), 1);
        }
        setenv("FFMPEG_STUDIO_SSH_CONFIG", pathToUtf8(sshConfig).c_str(), 1);

        const auto remoteTemporaryDirectories = []() {
            std::vector<fs::path> paths;
            std::error_code iteratorError;
            for (const auto& entry : fs::directory_iterator("/tmp", iteratorError)) {
                if (iteratorError) {
                    break;
                }
                if (entry.path().filename().string().starts_with("ffmpeg-studio-")) {
                    paths.push_back(entry.path());
                }
            }
            std::sort(paths.begin(), paths.end());
            return paths;
        };

        const auto temporaryDirectoriesBefore = remoteTemporaryDirectories();
        bool remoteProgressReported = false;
        bool remoteOutputVerified = false;

        const bool sshTestReady = useRealSsh ? realSshReady : scriptsExecutable;
        if (sshTestReady) {
            ffmpegui::FfmpegService remoteService;
            ffmpegui::SshServer remoteServer;
            remoteServer.alias = "ffmpeg-studio-self-test";
            remoteServer.hostName = useRealSsh ? "127.0.0.1" : "localhost";
            remoteServer.user = useRealSsh ? *realSshUser : "self-test";
            remoteServer.port = useRealSsh ? realPort : 22;
            remoteServer.identityFile = useRealSsh ? *realSshKey : "";
            remoteServer.authMethod = ffmpegui::SshAuthMethod::PublicKey;

            std::string sshError;
            const bool saved =
                remoteService.sshService().saveServer(remoteServer, {}, sshError);
            const bool connected = saved &&
                remoteService.sshService().testConnection(remoteServer.alias, sshError);
            const auto remoteProbe = connected
                ? remoteService.probeRemote(remoteServer.alias, false)
                : ffmpegui::RemoteProbeResult{};

            check(
                useRealSsh ? "SSH integration server configuration" : "SSH mock server configuration",
                saved);
            check(useRealSsh ? "SSH key authentication" : "SSH mock connection", connected);
            if (!connected && !sshError.empty()) {
                std::cout << "SSH connection detail: " << sshError << '\n';
            }
            check("SSH remote FFmpeg capability probe", remoteProbe.success);

            if (remoteProbe.success &&
                remoteProbe.capabilities.supports(
                    ffmpegui::VideoCodec::H264, ffmpegui::EncoderBackend::CPU)) {
                ffmpegui::ConversionSettings remoteSettings;
                remoteSettings.inputPath = pathToUtf8(source);
                remoteSettings.outputPath = pathToUtf8(work / "remote-encoded.mp4");
                remoteSettings.remoteHost = remoteServer.alias;
                remoteSettings.codec = ffmpegui::VideoCodec::H264;
                remoteSettings.backend = ffmpegui::EncoderBackend::CPU;
                remoteSettings.compression = ffmpegui::CompressionLevel::Fastest;
                remoteSettings.crf = 25;
                remoteSettings.audioBitrateKbps = 128;
                remoteSettings.threadsPerJob = 2;

                const auto remoteResult = remoteService.convert(
                    remoteSettings,
                    remoteProbe.capabilities,
                    [&](const ffmpegui::ConversionProgress& progress) {
                        remoteProgressReported |= progress.status == "正在远程编码";
                    },
                    nullptr,
                    nullptr);

                check("SSH remote video conversion", remoteResult.success);
                check("SSH remote progress reporting", remoteProgressReported);

                if (remoteResult.success) {
                    const auto verification = ffmpegui::runProcess({
                        service.ffprobePath(),
                        "-v", "error",
                        "-show_entries", "stream=codec_name",
                        "-of", "default=noprint_wrappers=1:nokey=1",
                        remoteResult.outputPath
                    });
                    remoteOutputVerified =
                        verification.exitCode == 0 &&
                        verification.output.find("h264") != std::string::npos;
                }
                check("SSH remote output download and verification", remoteOutputVerified);
            } else {
                std::cout << "SKIP SSH remote conversion: remote CPU H.264 encoder is unavailable\n";
            }

            if (useRealSsh) {
                ffmpegui::SshServer passwordServer = remoteServer;
                passwordServer.alias = "ffmpeg-studio-password-self-test";
                passwordServer.identityFile.clear();
                passwordServer.authMethod = ffmpegui::SshAuthMethod::Password;
                const bool passwordSaved = remoteService.sshService().saveServer(
                    passwordServer, *realSshPassword, sshError);
                check("SSH password credential storage", passwordSaved);
                const bool passwordConnected = passwordSaved &&
                    remoteService.sshService().testConnection(passwordServer.alias, sshError);
                if (!passwordConnected && !sshError.empty()) {
                    std::cout << "SSH password authentication detail: " << sshError << '\n';
                }
                check("SSH password authentication", passwordConnected);

                if (passwordConnected) {
                    const fs::path uploadPath = work / "ssh-password-upload.txt";
                    const fs::path downloadPath = work / "ssh-password-download.txt";
                    const std::string transferContent = "FFmpeg Studio SSH password transfer test";
                    {
                        std::ofstream output(uploadPath, std::ios::binary);
                        output << transferContent;
                    }
                    const std::string remotePath =
                        "/tmp/ffmpeg-studio-password-transfer-" +
                        std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch().count()) + ".txt";
                    const auto uploadResult = remoteService.sshService().upload(
                        passwordServer.alias, pathToUtf8(uploadPath), remotePath);
                    const auto downloadResult = uploadResult.exitCode == 0
                        ? remoteService.sshService().download(
                            passwordServer.alias, remotePath, pathToUtf8(downloadPath))
                        : ffmpegui::ProcessResult{};
                    std::ifstream downloadedFile(downloadPath, std::ios::binary);
                    const std::string downloadedContent(
                        std::istreambuf_iterator<char>(downloadedFile), {});
                    check(
                        "SSH password-authenticated file transfer",
                        uploadResult.exitCode == 0 && downloadResult.exitCode == 0 &&
                            downloadedContent == transferContent);
                    const auto cleanupResult = remoteService.sshService().runRemote(
                        passwordServer.alias,
                        "rm -f -- " + ffmpegui::SshService::shellQuote(remotePath));
                    check("SSH password test-file cleanup", cleanupResult.exitCode == 0);
                }
                (void)remoteService.sshService().removeServer(passwordServer.alias, sshError);
            }
            (void)remoteService.sshService().removeServer(remoteServer.alias, sshError);
        } else {
            check(useRealSsh ? "SSH integration server configuration" : "SSH mock command wrappers", false);
        }

        const auto temporaryDirectoriesAfter = remoteTemporaryDirectories();
        check("SSH remote temporary-directory cleanup",
              temporaryDirectoriesAfter == temporaryDirectoriesBefore);

        const auto restoreEnvironment = [](
            const char* name, const std::optional<std::string>& previousValue) {
            if (previousValue) {
                setenv(name, previousValue->c_str(), 1);
            } else {
                unsetenv(name);
            }
        };
        restoreEnvironment("FFMPEG_STUDIO_SSH_PATH", originalSshPath);
        restoreEnvironment("FFMPEG_STUDIO_SCP_PATH", originalScpPath);
        restoreEnvironment("FFMPEG_STUDIO_SSH_CONFIG", originalConfigPath);
    }
#endif

    {
        std::atomic_bool pauseRequested{false};
        std::atomic_bool processFinished{false};
        std::atomic<long long> mediaTimeUs{0};
        ffmpegui::ProcessResult pauseResult;
#ifdef _WIN32
        const std::string nullDevice = "NUL";
#else
        const std::string nullDevice = "/dev/null";
#endif
        const std::vector<std::string> pauseCommand{service.ffmpegPath(), "-hide_banner", "-loglevel", "error",
            "-re", "-f", "lavfi", "-i", "testsrc2=size=64x64:rate=30:duration=5",
            "-progress", "pipe:1", "-stats_period", "0.1", "-nostats", "-f", "null", nullDevice};
        std::thread pauseWorker([&] {
            pauseResult = ffmpegui::runProcess(pauseCommand, nullptr, [&](std::string_view line) {
                if (line.rfind("out_time_us=", 0) == 0) {
                    try { mediaTimeUs.store(std::stoll(std::string(line.substr(12)))); } catch (...) {}
                }
            }, &pauseRequested);
            processFinished.store(true);
        });
        const auto progressDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (mediaTimeUs.load() < 500000 && !processFinished.load() &&
               std::chrono::steady_clock::now() < progressDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        pauseRequested.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const long long pausedAt = mediaTimeUs.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const long long afterPause = mediaTimeUs.load();
        pauseRequested.store(false);
        pauseWorker.join();
        check("FFmpeg process pause and resume", pauseResult.exitCode == 0 && !pauseResult.cancelled &&
            pausedAt > 0 && afterPause - pausedAt <= 200000 && processFinished.load());
    }

    if (capabilities.supports(ffmpegui::VideoCodec::H264, ffmpegui::EncoderBackend::CPU)) {
        const fs::path movedVideo = work / "clip.mov";
        fs::copy_file(source, movedVideo, fs::copy_options::overwrite_existing);
        const fs::path movedVideo2 = work / "clip2.mov";
        fs::copy_file(source, movedVideo2, fs::copy_options::overwrite_existing);
        const fs::path movedVideo3 = work / "clip.avi";
        fs::copy_file(source, movedVideo3, fs::copy_options::overwrite_existing);
        const fs::path htmlReference = work / "index.html";
        { std::ofstream f(htmlReference); f << "<video src=\"media/clip.mov\"></video>"; }
        const fs::path zipInput = work / "sample.zip";
        const fs::path zipOutput = work / "sample_compressed.zip";
        check("create ZIP test fixture", createZip(zipInput, {{"media/clip.mov", movedVideo},
            {"media/clip2.mov", movedVideo2}, {"media/clip.avi", movedVideo3},
            {"notes/readme.txt", source}, {"notes/index.html", htmlReference}}));
        ffmpegui::ConversionSettings settings;
        settings.inputPath = pathToUtf8(movedVideo); settings.outputPath = pathToUtf8(work / "archive-temp.mp4");
        settings.codec = ffmpegui::VideoCodec::H264; settings.backend = ffmpegui::EncoderBackend::CPU;
        settings.compression = ffmpegui::CompressionLevel::Fastest; settings.crf = 23; settings.audioBitrateKbps = 128;
        ffmpegui::ArchiveService archives(service);
        ffmpegui::ArchiveProgressSnapshot zipProgress;
        const auto zipResult = archives.process(pathToUtf8(zipInput), pathToUtf8(zipOutput), false,
            settings, capabilities, 2, 2, [&](const auto& progress) { zipProgress = progress; }, nullptr);
        check("ZIP video compression and archive rebuild", zipResult.success && zipResult.processedVideos == 3 &&
            zipResult.inputSizeBytes > 0 && zipResult.outputSizeBytes > 0);
        const auto zipVideo = readZipEntry(zipOutput, "media/clip.mp4");
        check("ZIP internal video renamed to MP4", zipVideo.has_value() &&
            readZipEntry(zipOutput, "media/clip2.mp4").has_value());
        check("ZIP duplicate output names receive unique paths",
            readZipEntry(zipOutput, "media/clip_compressed.mp4").has_value());
        check("ZIP internal videos processed in parallel", zipProgress.items.size() == 3 &&
            std::count_if(zipProgress.items.begin(), zipProgress.items.end(), [](const auto& item) {
                return item.state == ffmpegui::TaskState::Completed;
            }) == 3);
        const auto updatedHtml = readZipEntry(zipOutput, "notes/index.html");
        check("ZIP text reference updated", updatedHtml &&
            (updatedHtml->find("media/clip.mp4") != std::string::npos ||
             updatedHtml->find("media/clip_compressed.mp4") != std::string::npos));
        check("ZIP non-media file preserved", readZipEntry(zipOutput, "notes/readme.txt").has_value());
        const auto zipSkip = archives.process(pathToUtf8(zipInput), pathToUtf8(zipOutput), false,
            settings, capabilities, 2, 2, {}, nullptr);
        check("ZIP same-parameter repeat skip", zipSkip.success && zipSkip.skipped);

        const fs::path pptxVideo = work / "pptx-clip.mov";
        fs::copy_file(source, pptxVideo, fs::copy_options::overwrite_existing);
        const fs::path pptxInput = work / "presentation.pptx";
        const fs::path pptxOutput = work / "presentation_compressed.pptx";
        const fs::path types = work / "types.xml";
        const fs::path presentation = work / "presentation.xml";
        const fs::path relationships = work / "slide1.xml.rels";

        {
            std::ofstream output(types);
            output
                << "<?xml version=\"1.0\"?>"
                << "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                << "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                << "</Types>";
        }
        {
            std::ofstream output(presentation);
            output
                << "<?xml version=\"1.0\"?>"
                << "<p:presentation "
                << "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"/>";
        }
        {
            std::ofstream output(relationships);
            output
                << "<?xml version=\"1.0\"?>"
                << "<Relationships>"
                << "<Relationship Id=\"rId1\" Type=\"video\" "
                << "Target=\"../media/clip.mov\"/>"
                << "<Relationship Id=\"rId2\" Type=\"video\" "
                << "Target=\"../media/clip2.mov\"/>"
                << "</Relationships>";
        }
        check("create PPTX test fixture", createZip(pptxInput, {
            {"[Content_Types].xml", types}, {"ppt/presentation.xml", presentation},
            {"ppt/slides/_rels/slide1.xml.rels", relationships}, {"ppt/media/clip.mov", pptxVideo},
            {"ppt/media/clip2.mov", movedVideo2}}));
        const auto pptxResult = archives.process(pathToUtf8(pptxInput), pathToUtf8(pptxOutput), true,
            settings, capabilities, 2, 2, {}, nullptr);
        check("PPTX embedded video conversion", pptxResult.success && pptxResult.processedVideos == 2 &&
            pptxResult.inputSizeBytes > 0 && pptxResult.outputSizeBytes > 0);
        const auto updatedRels = readZipEntry(pptxOutput, "ppt/slides/_rels/slide1.xml.rels");
        check("PPTX video relationships updated", updatedRels &&
            updatedRels->find("../media/clip.mp4") != std::string::npos &&
            updatedRels->find("../media/clip2.mp4") != std::string::npos);
        const auto updatedTypes = readZipEntry(pptxOutput, "[Content_Types].xml");
        check(
            "PPTX MP4 content type added",
            updatedTypes &&
                updatedTypes->find("Extension=\"mp4\"") != std::string::npos);
    }

    const auto plan = ffmpegui::recommendResourcePlan(12, ffmpegui::EncoderBackend::CPU,
        ffmpegui::VideoCodec::H264, 0);
    check("resource-aware parallel scheduling", plan.parallelJobs >= 1 && plan.threadsPerJob >= 1 &&
        plan.parallelJobs * plan.threadsPerJob <= plan.logicalCores + plan.parallelJobs);
    const auto limitedPlan = ffmpegui::recommendResourcePlan(12, ffmpegui::EncoderBackend::CPU,
        ffmpegui::VideoCodec::H264, 2);
    check("manual parallel limit", limitedPlan.parallelJobs <= 2);

    if (capabilities.supports(ffmpegui::VideoCodec::H264, ffmpegui::EncoderBackend::CPU)) {
        const fs::path batchInput = work / "batch-input";
        const fs::path batchOutput = work / "batch-output";
        const fs::path batchSource = work / "batch-source.mp4";
        const auto batchGenerated = ffmpegui::runProcess({service.ffmpegPath(), "-hide_banner", "-loglevel", "error",
            "-y", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30:duration=8",
            "-c:v", "mpeg4", "-q:v", "5", "-an", pathToUtf8(batchSource)});
        check("create batch scheduling media", batchGenerated.exitCode == 0);
        fs::create_directories(batchInput / "nested");
        fs::copy_file(batchSource, batchInput / "one.mp4", fs::copy_options::overwrite_existing);
        fs::copy_file(batchSource, batchInput / "two.mp4", fs::copy_options::overwrite_existing);
        fs::copy_file(batchSource, batchInput / "nested" / "three.mp4", fs::copy_options::overwrite_existing);
        ffmpegui::AppController batch(service);
        batch.initialize();
        auto batchSettings = batch.snapshot().settings;
        batchSettings.codec = ffmpegui::VideoCodec::H264;
        batchSettings.backend = ffmpegui::EncoderBackend::CPU;
        batchSettings.compression = ffmpegui::CompressionLevel::Slow;
        batchSettings.crf = 30;
        batchSettings.audioBitrateKbps = 128;
        batchSettings.maxParallelJobs = 2;
        batchSettings.outputDirectory = pathToUtf8(batchOutput);
        batchSettings.recursive = true;
        batchSettings.preserveDirectoryStructure = true;
        batch.updateSettings(batchSettings);
        batch.addInputs({pathToUtf8(batchInput)});
        check("recursive folder discovery", batch.snapshot().totalTasks == 3);
        const bool started = batch.startBatch();
        std::size_t maxRunning = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (started && batch.snapshot().running && std::chrono::steady_clock::now() < deadline) {
            const auto state = batch.snapshot();
            maxRunning = std::max(maxRunning, state.runningTasks);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto batchResult = batch.snapshot();
        check("parallel folder batch completion", batchResult.completedTasks == 3 && batchResult.failedTasks == 0);
        const auto expectedWorkers = std::min<std::size_t>(2, batch.snapshot().resourcePlan.parallelJobs);
        std::cout << "Observed concurrent batch tasks: "
                  << maxRunning << " / " << expectedWorkers
                  << " expected workers\n";
        check("parallel worker count follows resource plan", maxRunning >= expectedWorkers && maxRunning <= 2);
        check("custom output directory and folder structure",
            fs::is_regular_file(batchOutput / "one_H264_CPU_CR30_慢速_A128.mp4") &&
            fs::is_regular_file(batchOutput / "nested" / "three_H264_CPU_CR30_慢速_A128.mp4"));
        batch.shutdown();

        ffmpegui::AppController repeatBatch(service);
        repeatBatch.initialize();
        repeatBatch.updateSettings(batchSettings);
        repeatBatch.addInputs({pathToUtf8(batchInput)});
        const bool repeatStarted = repeatBatch.startBatch();
        const auto repeatDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (repeatStarted && repeatBatch.snapshot().running && std::chrono::steady_clock::now() < repeatDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto repeatResult = repeatBatch.snapshot();
        check(
            "repeat folder batch skips existing outputs",
            repeatResult.skippedTasks == 3 && repeatResult.failedTasks == 0);
        repeatBatch.shutdown();

        const fs::path sameNameA = work / "same-name-a" / "duplicate.mov";
        const fs::path sameNameB = work / "same-name-b" / "duplicate.mov";
        fs::create_directories(sameNameA.parent_path());
        fs::create_directories(sameNameB.parent_path());
        fs::copy_file(batchSource, sameNameA, fs::copy_options::overwrite_existing);
        fs::copy_file(batchSource, sameNameB, fs::copy_options::overwrite_existing);
        ffmpegui::AppController collisionBatch(service);
        collisionBatch.initialize();
        auto collisionSettings = collisionBatch.snapshot().settings;
        collisionSettings.codec = ffmpegui::VideoCodec::H264;
        collisionSettings.backend = ffmpegui::EncoderBackend::CPU;
        collisionSettings.outputDirectory = pathToUtf8(work / "collision-output");
        collisionBatch.updateSettings(collisionSettings);
        collisionBatch.addInputs({pathToUtf8(sameNameA), pathToUtf8(sameNameB)});
        const auto collisionSnapshot = collisionBatch.snapshot();
        check("same-name inputs receive distinct output paths", collisionSnapshot.tasks.size() == 2 &&
            collisionSnapshot.tasks[0].outputPath != collisionSnapshot.tasks[1].outputPath);
        collisionSettings.crf = 29;
        collisionBatch.updateSettings(collisionSettings);
        const auto refreshedCollision = collisionBatch.snapshot();
        check("output path updates preserve unique names", refreshedCollision.tasks.size() == 2 &&
            refreshedCollision.tasks[0].outputPath != refreshedCollision.tasks[1].outputPath);
        collisionBatch.shutdown();
        batch.updateSettings(batchSettings);

        const fs::path queueFile = work / "batch.ffq";
        std::string queueError;
        check("export batch task list", batch.exportQueue(pathToUtf8(queueFile), queueError));
        ffmpegui::AppController imported(service);
        const auto importedCount = imported.importQueue(pathToUtf8(queueFile), queueError);
        check(
            "import batch task list",
            queueError.empty() &&
                importedCount == 3 &&
                imported.snapshot().totalTasks == 3);
        auto savedSettings = batch.snapshot().settings;
        ffmpegui::AppController settingsReloaded(service);
        const auto reloaded = settingsReloaded.snapshot().settings;
        check("persist encoding settings and output directory", reloaded.codec == savedSettings.codec &&
            reloaded.backend == savedSettings.backend && reloaded.crf == savedSettings.crf &&
            reloaded.audioBitrateKbps == savedSettings.audioBitrateKbps &&
            reloaded.outputDirectory == savedSettings.outputDirectory);
    }

    const fs::path maliciousZip = work / "unsafe-path.zip";
    const fs::path unsafeOutput = work / "unsafe-output.zip";
    if (fs::is_regular_file(source)) {
        check("create ZIP path traversal fixture", createZip(maliciousZip, {{"../outside.txt", source}}));
        ffmpegui::ConversionSettings settings;
        settings.inputPath = pathToUtf8(source); settings.outputPath = pathToUtf8(work / "unused.mp4");
        settings.codec = ffmpegui::VideoCodec::H264; settings.backend = ffmpegui::EncoderBackend::CPU;
        settings.compression = ffmpegui::CompressionLevel::Fastest; settings.crf = 23; settings.audioBitrateKbps = 128;
        ffmpegui::ArchiveService archives(service);
        const auto unsafeResult = archives.process(pathToUtf8(maliciousZip), pathToUtf8(unsafeOutput), false,
            settings, capabilities, 1, 1, {}, nullptr);
        check("ZIP path traversal is rejected", !unsafeResult.success && !fs::exists(unsafeOutput));
    }

    std::cout << "\nSelf-test result: " << passed << " passed, " << failed << " failed.\n";
#ifdef _WIN32
    _putenv_s("FFMPEG_STUDIO_CONFIG_DIR", "");
#else
    unsetenv("FFMPEG_STUDIO_CONFIG_DIR");
#endif
    if (keepFiles) std::cout << "Generated test files: " << pathToUtf8(work) << '\n';
    else { std::error_code ignored; fs::remove_all(work, ignored); }
    return failed == 0 && passed > 0 ? 0 : 1;
}

int runSshAskpass() {
    const auto alias = ffmpegui::environmentVariable("FFMPEG_STUDIO_SSH_ASKPASS_ALIAS");
    if (!alias || alias->empty()) {
        return 1;
    }

    ffmpegui::SshService ssh;
    auto password = ssh.passwordForAskpass(*alias);
    if (!password || password->empty()) return 1;
    bool written = false;
#ifdef _WIN32
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output != nullptr && output != INVALID_HANDLE_VALUE) {
        DWORD bytes = 0;
        written = WriteFile(output, password->data(), static_cast<DWORD>(password->size()), &bytes, nullptr) &&
                  bytes == password->size();
        if (written) written = WriteFile(output, "\n", 1, &bytes, nullptr) && bytes == 1;
    }
#else
    const auto count = ::write(STDOUT_FILENO, password->data(), password->size());
    written = count == static_cast<ssize_t>(password->size());
    if (written) written = ::write(STDOUT_FILENO, "\n", 1) == 1;
#endif
    volatile char* secret = password->data();
    for (std::size_t i = 0; i < password->size(); ++i) secret[i] = 0;
    return written ? 0 : 1;
}

int appMain(const std::vector<std::string>& arguments) {
    auto service = makeService();
    if (arguments.size() > 1) {
        const std::string& command = arguments[1];
        if (command == "--help" || command == "-h") { printUsage(); return 0; }
        if (command == "--demo") return runSelfTest(true);
        if (command == "--self-test") return runSelfTest(false);
        if (command == "--encoders") return listEncoders(service.probeCapabilities(true));
    }

    std::string outputDirectory;
    std::vector<std::string> inputs;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == "--output-dir" && index + 1 < arguments.size()) {
            outputDirectory = arguments[++index];
        } else if (arguments[index] == "--") {
            for (++index; index < arguments.size(); ++index) inputs.push_back(arguments[index]);
            break;
        } else if (!arguments[index].empty() && arguments[index][0] == '-') {
            std::cerr << "Unknown argument: " << arguments[index] << '\n';
            printUsage();
            return 2;
        } else inputs.push_back(arguments[index]);
    }

#ifdef _WIN32
    if (const HWND console = GetConsoleWindow(); console != nullptr) ShowWindow(console, SW_HIDE);
#endif
    ffmpegui::AppController controller(std::move(service));
    controller.initialize();
    if (!outputDirectory.empty()) {
        auto settings = controller.snapshot().settings;
        settings.outputDirectory = outputDirectory;
        controller.updateSettings(settings);
    }
    if (!inputs.empty()) controller.addInputs(inputs);
    return ffmpegui::runNativeUi(controller);
}

} // namespace

int main(int argc, char** argv) {
    if (ffmpegui::environmentVariable("FFMPEG_STUDIO_SSH_ASKPASS_ALIAS")) {
        return runSshAskpass();
    }
    std::vector<std::string> arguments;
#ifdef _WIN32
    int count = 0;
    LPWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wideArgs != nullptr) {
        arguments.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) arguments.push_back(ffmpegui::utf8FromWide(wideArgs[i]));
        LocalFree(wideArgs);
    }
#else
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) arguments.emplace_back(argv[i]);
#endif
    return appMain(arguments);
}
