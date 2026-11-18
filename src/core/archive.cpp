#include "archive.hpp"

#include "process.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ffmpegui {
namespace {
namespace fs = std::filesystem;

std::string pathToUtf8(const fs::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

fs::path pathFromUtf8(std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character : value) encoded.push_back(static_cast<char8_t>(character));
    return fs::path(encoded);
}

std::string genericPathToUtf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

int openArchiveReader(archive* reader, const fs::path& path) {
#ifdef _WIN32
    return archive_read_open_filename_w(reader, path.c_str(), 10240);
#else
    const std::string nativePath = path.string();
    return archive_read_open_filename(reader, nativePath.c_str(), 10240);
#endif
}

int openArchiveWriter(archive* writer, const fs::path& path) {
#ifdef _WIN32
    return archive_write_open_filename_w(writer, path.c_str());
#else
    const std::string nativePath = path.string();
    return archive_write_open_filename(writer, nativePath.c_str());
#endif
}

std::string archiveEntryPath(archive_entry* entry) {
    if (const char* utf8 = archive_entry_pathname_utf8(entry))
        return std::string(utf8);
#ifdef _WIN32
    if (const wchar_t* wide = archive_entry_pathname_w(entry))
        return pathToUtf8(fs::path(wide));
#endif
    if (const char* name = archive_entry_pathname(entry))
        return std::string(name);
    return {};
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}


std::string archiveError(archive* handle, const std::string& fallback) {
    const char* message = handle == nullptr ? nullptr : archive_error_string(handle);
    return message == nullptr ? fallback : std::string(message);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("ffmpegui-archive-" +
            std::to_string(static_cast<unsigned long long>(stamp + sequence.fetch_add(1))));
        fs::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    const fs::path& path() const { return path_; }
private:
    fs::path path_;
};

bool safeArchivePath(std::string name, fs::path& relative) {
    if (name.empty()) return false;
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.front() == '/' || name.find(':') != std::string::npos) return false;
    std::istringstream parts(name);
    std::string component;
    while (std::getline(parts, component, '/')) {
        if (component == ".." || component == ".") return false;
    }
    relative = pathFromUtf8(name).lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_name()) return false;
    return true;
}

bool extractArchive(const fs::path& source, const fs::path& destination, std::string& error) {
    archive* reader = archive_read_new();
    if (reader == nullptr) {
        error = "无法创建 ZIP 读取器。";
        return false;
    }
    archive_read_support_filter_all(reader);
    archive_read_support_format_zip(reader);
    if (openArchiveReader(reader, source) != ARCHIVE_OK) {
        error = "无法打开 ZIP 文件：" + archiveError(reader, "未知错误");
        archive_read_free(reader);
        return false;
    }

    constexpr std::uint64_t maxTotalBytes = 50ULL * 1024ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t maxEntryBytes = 12ULL * 1024ULL * 1024ULL * 1024ULL;
    constexpr std::size_t maxEntries = 100000;
    std::uint64_t totalBytes = 0;
    std::size_t entries = 0;
    archive_entry* entry = nullptr;
    bool success = true;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        if (++entries > maxEntries) {
            error = "ZIP 文件包含过多条目，已停止处理。";
            success = false;
            break;
        }
        const std::string rawName = archiveEntryPath(entry);
        fs::path relative;
        if (rawName.empty() || !safeArchivePath(rawName, relative)) {
            error = "ZIP 文件包含不安全的路径，已停止处理。";
            success = false;
            break;
        }
        const fs::path output = destination / relative;
        const auto type = archive_entry_filetype(entry);
        if (type == AE_IFLNK || archive_entry_symlink(entry) != nullptr || archive_entry_hardlink(entry) != nullptr) {
            error = "ZIP 文件包含符号链接，出于安全考虑已停止处理。";
            success = false;
            break;
        }
        if (type == AE_IFDIR) {
            fs::create_directories(output);
            archive_read_data_skip(reader);
            continue;
        }
        if (type != AE_IFREG && type != 0) {
            archive_read_data_skip(reader);
            continue;
        }
        const la_int64_t declaredSize = archive_entry_size(entry);
        if (declaredSize < 0 || static_cast<std::uint64_t>(declaredSize) > maxEntryBytes ||
            totalBytes + static_cast<std::uint64_t>(declaredSize) > maxTotalBytes) {
            error = "ZIP 文件展开后的数据量超过安全限制。";
            success = false;
            break;
        }
        totalBytes += static_cast<std::uint64_t>(declaredSize);
        std::error_code fsError;
        fs::create_directories(output.parent_path(), fsError);
        if (fsError) {
            error = "无法创建 ZIP 内部目录：" + fsError.message();
            success = false;
            break;
        }
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "无法写入 ZIP 内部文件：" + pathToUtf8(relative);
            success = false;
            break;
        }
        std::array<char, 128 * 1024> buffer{};
        la_ssize_t bytes = 0;
        std::uint64_t actualSize = 0;
        while ((bytes = archive_read_data(reader, buffer.data(), buffer.size())) > 0) {
            actualSize += static_cast<std::uint64_t>(bytes);
            const std::uint64_t declaredBytes =
                static_cast<std::uint64_t>(declaredSize);
            const std::uint64_t projectedTotal =
                totalBytes - declaredBytes + actualSize;
            if (actualSize > maxEntryBytes || projectedTotal > maxTotalBytes) {
                error = "ZIP 文件实际展开的数据量超过安全限制。";
                success = false;
                break;
            }
            file.write(buffer.data(), bytes);
            if (!file) {
                error = "写入 ZIP 内部文件时发生错误。";
                success = false;
                break;
            }
        }
        if (bytes < 0) {
            error = "读取 ZIP 内部文件时发生错误：" + archiveError(reader, "未知错误");
            success = false;
        }
        file.close();
        if (!success) break;
        const auto permissions = static_cast<fs::perms>(archive_entry_perm(entry) & 0777);
        if (permissions != fs::perms::unknown) fs::permissions(output, permissions, fs::perm_options::replace, fsError);
        if (archive_entry_mtime_is_set(entry)) {
            const auto time = fs::file_time_type(std::chrono::seconds(archive_entry_mtime(entry)));
            fs::last_write_time(output, time, fsError);
        }
    }
    if (success && status != ARCHIVE_EOF) {
        error = "读取 ZIP 文件失败：" + archiveError(reader, "未知错误");
        success = false;
    }
    archive_read_close(reader);
    archive_read_free(reader);
    return success;
}

bool writeArchive(const fs::path& sourceDirectory, const fs::path& destination,
                  std::string& error) {
    archive* writer = archive_write_new();
    if (writer == nullptr) {
        error = "无法创建 ZIP 写入器。";
        return false;
    }
    archive_write_set_format_zip(writer);
    archive_write_set_format_option(writer, "zip", "compression", "deflate");
    if (archive_write_set_format_option(writer, "zip", "hdrcharset", "UTF-8") != ARCHIVE_OK) {
        error = "无法将 ZIP 文件名编码设置为 UTF-8：" + archiveError(writer, "未知错误");
        archive_write_free(writer);
        return false;
    }
    if (openArchiveWriter(writer, destination) != ARCHIVE_OK) {
        error = "无法创建输出 ZIP 文件：" + archiveError(writer, "未知错误");
        archive_write_free(writer);
        return false;
    }

    std::error_code fsError;
    bool success = true;
    for (fs::recursive_directory_iterator it(
             sourceDirectory, fs::directory_options::skip_permission_denied, fsError),
         end;
         it != end && !fsError;
         it.increment(fsError)) {
        if (it->is_symlink(fsError)) continue;
        const fs::path relative = fs::relative(it->path(), sourceDirectory, fsError);
        if (fsError) break;
        const std::string archiveName = genericPathToUtf8(relative);
        archive_entry* entry = archive_entry_new();
        if (entry == nullptr) {
            error = "无法创建 ZIP 条目。";
            success = false;
            break;
        }
        archive_entry_set_pathname_utf8(entry, archiveName.c_str());
        const bool directory = it->is_directory(fsError);
        const bool regular = it->is_regular_file(fsError);
        if (directory) {
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_perm(entry, 0755);
            archive_entry_set_size(entry, 0);
        } else if (regular) {
            archive_entry_set_filetype(entry, AE_IFREG);
            const auto size = it->file_size(fsError);
            archive_entry_set_size(entry, static_cast<la_int64_t>(size));
            archive_entry_set_perm(entry, 0644);
            const auto time = fs::last_write_time(it->path(), fsError);
            if (!fsError) {
                const auto systemTime = std::chrono::time_point_cast<std::chrono::seconds>(
                    time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                archive_entry_set_mtime(entry, systemTime.time_since_epoch().count(), 0);
            }
        } else {
            archive_entry_free(entry);
            continue;
        }
        if (archive_write_header(writer, entry) != ARCHIVE_OK) {
            error = "写入 ZIP 条目失败：" + archiveError(writer, archiveName);
            archive_entry_free(entry);
            success = false;
            break;
        }
        if (regular) {
            std::ifstream file(it->path(), std::ios::binary);
            std::array<char, 128 * 1024> buffer{};
            while (file) {
                file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = file.gcount();
                if (count > 0 && archive_write_data(writer, buffer.data(), static_cast<std::size_t>(count)) < 0) {
                    error = "写入 ZIP 文件数据失败：" + archiveError(writer, archiveName);
                    success = false;
                    break;
                }
            }
            if (!file.eof() && file.fail()) {
                error = "读取待打包文件失败：" + archiveName;
                success = false;
            }
        }
        archive_entry_free(entry);
        if (!success) break;
    }
    if (fsError && success) {
        error = "扫描待打包目录失败：" + fsError.message();
        success = false;
    }
    if (archive_write_close(writer) != ARCHIVE_OK && success) {
        error = "关闭 ZIP 文件失败：" + archiveError(writer, "未知错误");
        success = false;
    }
    archive_write_free(writer);
    return success;
}

bool readManifest(const fs::path& archivePath, const std::string& manifestName,
                  std::string& content) {
    archive* reader = archive_read_new();
    if (reader == nullptr) return false;
    archive_read_support_filter_all(reader);
    archive_read_support_format_zip(reader);
    if (openArchiveReader(reader, archivePath) != ARCHIVE_OK) {
        archive_read_free(reader);
        return false;
    }
    archive_entry* entry = nullptr;
    bool found = false;
    while (archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
        const std::string name = archiveEntryPath(entry);
        if (!name.empty() && manifestName == name) {
            std::array<char, 4096> buffer{};
            la_ssize_t bytes = 0;
            while ((bytes = archive_read_data(reader, buffer.data(), buffer.size())) > 0)
                content.append(buffer.data(), static_cast<std::size_t>(bytes));
            found = bytes >= 0;
            break;
        }
        archive_read_data_skip(reader);
    }
    archive_read_close(reader);
    archive_read_free(reader);
    return found;
}

std::string manifestContent(const fs::path& input, const ConversionSettings& settings) {
    std::error_code error;
    const auto size = fs::file_size(input, error);
    const auto modified = fs::last_write_time(input, error).time_since_epoch().count();
    std::ostringstream value;
    value << "FFmpegStudioArchive/1\n"
          << "fingerprint=" << settingsFingerprint(settings) << "\n"
          << "input_size=" << size << "\n"
          << "input_mtime=" << modified << "\n";
    return value.str();
}

bool manifestMatches(const fs::path& archivePath, const std::string& manifestName,
                     const fs::path& input, const ConversionSettings& settings) {
    std::string current;
    if (!readManifest(archivePath, manifestName, current)) return false;
    return current == manifestContent(input, settings);
}

std::string manifestPath(bool powerPoint) {
    return powerPoint ? "ppt/media/.ffmpegui-manifest.txt" : ".ffmpegui/manifest.txt";
}

std::vector<fs::path> findVideos(const fs::path& root) {
    std::vector<fs::path> videos;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
         it != end && !error; it.increment(error)) {
        if (it->is_regular_file(error) && isVideoFile(pathToUtf8(it->path())))
            videos.push_back(it->path());
    }
    std::sort(videos.begin(), videos.end());
    return videos;
}

std::string relativeName(const fs::path& path, const fs::path& root) {
    return genericPathToUtf8(fs::relative(path, root));
}

std::string replaceAll(std::string value, const std::string& from, const std::string& to) {
    if (from.empty()) return value;
    std::size_t position = 0;
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
    return value;
}

bool isTextReferenceFile(const fs::path& path) {
    static const std::vector<std::string> extensions{
        ".xml", ".rels", ".html", ".htm", ".json", ".txt", ".css", ".js", ".m3u8", ".md"};
    const std::string ext = lower(pathToUtf8(path.extension()));
    return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

void updateReferences(const fs::path& root,
                      const std::vector<std::pair<std::string, std::string>>& replacements,
                      bool powerPoint) {
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
         it != end && !error; it.increment(error)) {
        if (!it->is_regular_file(error) || !isTextReferenceFile(it->path())) continue;
        const auto size = it->file_size(error);
        if (error || size > 16ULL * 1024ULL * 1024ULL) continue;
        std::ifstream input(it->path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (text.empty()) continue;
        const std::string relativeFile = relativeName(it->path(), root);
        for (const auto& [oldPath, newPath] : replacements) {
            text = replaceAll(std::move(text), oldPath, newPath);
            if (powerPoint && relativeFile.size() >= 5 && relativeFile.substr(relativeFile.size() - 5) == ".rels") {
                const fs::path relsPath = pathFromUtf8(relativeFile);
                const fs::path sourceDirectory = relsPath.parent_path().parent_path();
                const fs::path oldTarget = pathFromUtf8(oldPath).lexically_relative(sourceDirectory);
                const fs::path newTarget = pathFromUtf8(newPath).lexically_relative(sourceDirectory);
                text = replaceAll(std::move(text), genericPathToUtf8(oldTarget),
                                  genericPathToUtf8(newTarget));
                text = replaceAll(std::move(text), "/" + oldPath, "/" + newPath);
            }
        }
        if (powerPoint && relativeFile == "[Content_Types].xml") {
            for (const auto& [oldPath, newPath] : replacements) {
                (void)oldPath;
                const std::string extension = lower(pathToUtf8(pathFromUtf8(newPath).extension()).substr(1));
                if (extension.empty()) continue;
                const std::string tokenDouble = "Extension=\"" + extension + "\"";
                const std::string tokenSingle = "Extension='" + extension + "'";
                if (text.find(tokenDouble) != std::string::npos ||
                    text.find(tokenSingle) != std::string::npos) {
                    continue;
                }
                std::string contentType = "video/mp4";
                if (extension == "mov") contentType = "video/quicktime";
                else if (extension == "wmv") contentType = "video/x-ms-wmv";
                else if (extension == "webm") contentType = "video/webm";
                else if (extension == "avi") contentType = "video/x-msvideo";
                else if (extension == "mkv") contentType = "video/x-matroska";
                const auto closing = text.rfind("</Types>");
                if (closing != std::string::npos) {
                    const std::string defaultEntry =
                        "<Default Extension=\"" + extension +
                        "\" ContentType=\"" + contentType + "\"/>";
                    text.insert(closing, defaultEntry);
                }
            }
        }
        std::ofstream output(it->path(), std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

bool validateArchive(const fs::path& path, bool powerPoint, std::string& error) {
    TemporaryDirectory checkDirectory;
    if (!extractArchive(path, checkDirectory.path(), error)) return false;
    if (powerPoint) {
        if (!fs::is_regular_file(checkDirectory.path() / "[Content_Types].xml") ||
            !fs::is_regular_file(checkDirectory.path() / "ppt/presentation.xml")) {
            error = "生成的 PPTX 缺少必要的 Office 文档结构。";
            return false;
        }
    }
    return true;
}

bool replaceArchiveEntryFile(const fs::path& source, const fs::path& destination,
                             std::string& error) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "无法替换压缩包内的视频文件。Windows 错误代码：" + std::to_string(GetLastError());
        return false;
    }
    return true;
#else
    std::error_code fsError;
    fs::rename(source, destination, fsError);
    if (fsError) {
        error = "无法替换压缩包内的视频文件：" + fsError.message();
        return false;
    }
    return true;
#endif
}

bool replaceOutput(const fs::path& temporary, const fs::path& output, bool overwrite,
                   std::string& error) {
#ifdef _WIN32
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(temporary.c_str(), output.c_str(), flags)) {
        error = "无法保存输出压缩包。Windows 错误代码：" + std::to_string(GetLastError());
        return false;
    }
    return true;
#else
    std::error_code fsError;
    if (!overwrite && fs::exists(output, fsError)) {
        error = "输出文件已经存在。";
        return false;
    }
    fs::rename(temporary, output, fsError);
    if (fsError) {
        error = "无法保存输出压缩包：" + fsError.message();
        return false;
    }
    return true;
#endif
}

} // namespace

ArchiveService::ArchiveService(const FfmpegService& ffmpeg) : ffmpeg_(ffmpeg) {}

ArchiveResult ArchiveService::process(const std::string& inputPath,
                                       const std::string& outputPath,
                                       bool isPowerPoint,
                                       const ConversionSettings& settings,
                                       const EncoderCapabilities& capabilities,
                                       unsigned int parallelJobs,
                                       unsigned int threadsPerJob,
                                       const ArchiveProgressCallback& onProgress,
                                       const std::atomic_bool* cancelRequested,
                                       const std::atomic_bool* pauseRequested) const {
    ArchiveResult result;
    result.outputPath = outputPath;
    const fs::path input = pathFromUtf8(inputPath);
    const fs::path output = pathFromUtf8(outputPath);
    const std::string marker = manifestPath(isPowerPoint);
    std::error_code fsError;
    if (!fs::is_regular_file(input, fsError)) {
        result.message = "找不到待处理的压缩包。";
        return result;
    }
    result.inputSizeBytes = fs::file_size(input, fsError);
    // PowerPoint media playback is reliably supported with MP4. Allow
    // the existing source-container mode for legacy presentations.
    if (isPowerPoint && (settings.outputContainer == OutputContainer::MKV ||
                         settings.outputContainer == OutputContainer::WebM)) {
        result.message = "PowerPoint 内嵌视频请使用 MP4 输出或选择原格式。";
        return result;
    }
    if (manifestMatches(output, marker, input, settings)) {
        std::string validationError;
        if (validateArchive(output, isPowerPoint, validationError)) {
            result.success = true;
            result.skipped = true;
            result.message = "压缩包已经使用相同参数处理，已自动跳过。";
            result.outputSizeBytes = fs::file_size(output, fsError);
            return result;
        }
    }
    if (fs::exists(output, fsError) && !settings.overwrite) {
        result.message = "输出压缩包已经存在。请修改输出位置或确认覆盖操作。";
        return result;
    }
    fs::create_directories(output.parent_path().empty() ? fs::current_path() : output.parent_path(), fsError);
    if (fsError) {
        result.message = "无法创建输出文件夹：" + fsError.message();
        return result;
    }

    TemporaryDirectory temporary;
    const fs::path extracted = temporary.path() / "content";
    fs::create_directories(extracted, fsError);
    std::string error;
    if (!extractArchive(input, extracted, error)) {
        result.message = error;
        return result;
    }
    if (isPowerPoint && (!fs::is_regular_file(extracted / "[Content_Types].xml") ||
                         !fs::is_regular_file(extracted / "ppt/presentation.xml"))) {
        result.message = "该文件不是有效的 PowerPoint PPTX 文档。";
        return result;
    }

    const auto videos = findVideos(extracted);
    result.totalVideos = videos.size();
    if (videos.empty()) {
        result.success = true;
        result.skipped = true;
        result.message = "压缩包中没有可处理的视频文件。";
        return result;
    }

    std::vector<fs::path> targetRelativePaths(videos.size());
    std::set<std::string> reservedTargets;
    for (std::size_t index = 0; index < videos.size(); ++index) {
        const std::string oldRelative = relativeName(videos[index], extracted);
        fs::path candidate = pathFromUtf8(oldRelative);
        if (settings.outputContainer != OutputContainer::SameAsSource)
            candidate.replace_extension(
                outputVideoExtension(settings.outputContainer, oldRelative));
        auto targetKey = [](const fs::path& value) {
            return lower(genericPathToUtf8(value.lexically_normal()));
        };
        if (candidate != pathFromUtf8(oldRelative) &&
            (reservedTargets.contains(targetKey(candidate)) || fs::exists(extracted / candidate))) {
            const std::string stem = pathToUtf8(candidate.stem());
            const fs::path parent = candidate.parent_path();
            unsigned int duplicate = 0;
            do {
                ++duplicate;
                const std::string suffix = duplicate == 1 ? "_compressed" : "_compressed_" + std::to_string(duplicate);
                candidate = parent / pathFromUtf8(stem + suffix + pathToUtf8(candidate.extension()));
            } while (reservedTargets.contains(targetKey(candidate)) || fs::exists(extracted / candidate));
        }
        reservedTargets.insert(targetKey(candidate));
        targetRelativePaths[index] = candidate;
    }

    ArchiveProgressSnapshot snapshot;
    snapshot.status = "正在处理压缩包";
    snapshot.detail = "已发现 " + std::to_string(videos.size()) + " 个视频文件";
    snapshot.items.reserve(videos.size());
    for (const auto& video : videos) {
        ArchiveItemProgress item;
        item.name = relativeName(video, extracted);
        item.detail = "等待处理";
        snapshot.items.push_back(std::move(item));
    }
    if (onProgress) onProgress(snapshot);

    std::vector<std::pair<std::string, std::string>> replacements;
    std::mutex progressMutex;
    std::mutex resultMutex;
    std::atomic_size_t nextIndex{0};
    std::atomic_bool failed{false};
    std::atomic_bool wasCancelled{false};
    std::string firstError;
    std::mutex errorMutex;
    const unsigned int workerCount = std::max(1U, std::min<unsigned int>(
        static_cast<unsigned int>(videos.size()), std::max(1U, parallelJobs)));
    const fs::path outputDirectory = temporary.path() / "encoded";
    fs::create_directories(outputDirectory, fsError);

    auto publish = [&](std::size_t index, const ConversionProgress* progress,
                       TaskState state, const std::string& detail,
                       std::uint64_t inputBytes = 0, std::uint64_t outputBytes = 0) {
        ArchiveProgressSnapshot copy;
        {
            std::lock_guard lock(progressMutex);
            auto& item = snapshot.items[index];
            if (progress != nullptr) item.progress = *progress;
            item.state = state;
            if (state == TaskState::Completed || state == TaskState::Skipped) item.progress.fraction = 1.0;
            if (inputBytes > 0) item.inputSizeBytes = inputBytes;
            if (outputBytes > 0) item.outputSizeBytes = outputBytes;
            item.detail = detail;
            double sum = 0.0;
            std::size_t completed = 0;
            for (const auto& current : snapshot.items) {
                sum += current.progress.fraction;
                if (current.state == TaskState::Completed || current.state == TaskState::Skipped) ++completed;
            }
            snapshot.fraction = snapshot.items.empty() ? 0.0 : sum / static_cast<double>(snapshot.items.size());
            snapshot.status = "正在处理压缩包";
            snapshot.detail = std::to_string(completed) + " / " + std::to_string(snapshot.items.size()) + " 个视频已完成";
            copy = snapshot;
        }
        if (onProgress) onProgress(copy);
    };

    auto worker = [&]() {
        while (!failed.load() && !(cancelRequested && cancelRequested->load())) {
            const std::size_t index = nextIndex.fetch_add(1);
            if (index >= videos.size()) break;
            const fs::path source = videos[index];
            const std::string oldRelative = relativeName(source, extracted);
            const fs::path newRelative = targetRelativePaths[index];
            const std::string encodedExtension = outputVideoExtension(settings.outputContainer, oldRelative);
            const fs::path encodedPath = outputDirectory / pathFromUtf8(std::to_string(index) + encodedExtension);
            ConversionSettings perVideo = settings;
            perVideo.inputPath = pathToUtf8(source);
            perVideo.outputPath = pathToUtf8(encodedPath);
            perVideo.overwrite = false;
            perVideo.threadsPerJob = static_cast<int>(threadsPerJob);
            perVideo.skipAlreadyProcessed = settings.skipAlreadyProcessed;

            publish(index, nullptr, TaskState::Running, "正在编码");
            const ConversionResult conversion = ffmpeg_.convert(perVideo, capabilities,
                [&](const ConversionProgress& progress) {
                    publish(index, &progress, TaskState::Running, progress.detail);
                }, cancelRequested, pauseRequested);
            if (conversion.cancelled) {
                wasCancelled.store(true);
                publish(index, nullptr, TaskState::Cancelled, "已取消");
                break;
            }
            if (!conversion.success) {
                failed.store(true);
                {
                    std::lock_guard lock(errorMutex);
                    if (firstError.empty()) firstError = conversion.message;
                }
                publish(index, nullptr, TaskState::Failed, conversion.message);
                break;
            }
            if (conversion.skipped) {
                { std::lock_guard lock(resultMutex); ++result.skippedVideos; }
                publish(index, nullptr, TaskState::Skipped, conversion.message,
                        conversion.inputSizeBytes, conversion.outputSizeBytes);
                continue;
            }
            const fs::path target = extracted / newRelative;
            std::error_code moveError;
            fs::create_directories(target.parent_path(), moveError);
            if (moveError) {
                failed.store(true);
                std::lock_guard lock(errorMutex);
                if (firstError.empty()) firstError = "无法创建媒体文件目录：" + moveError.message();
                publish(index, nullptr, TaskState::Failed, firstError);
                break;
            }
            std::string replaceError;
            if (!replaceArchiveEntryFile(encodedPath, target, replaceError)) {
                failed.store(true);
                std::lock_guard lock(errorMutex);
                if (firstError.empty()) firstError = replaceError;
                publish(index, nullptr, TaskState::Failed, firstError);
                break;
            }
            if (source != target) fs::remove(source, moveError);
            if (source != target) {
                std::lock_guard lock(resultMutex);
                replacements.emplace_back(oldRelative, genericPathToUtf8(newRelative));
            }
            { std::lock_guard lock(resultMutex); ++result.processedVideos; }
            ConversionProgress done;
            done.fraction = 1.0;
            done.status = "已完成";
            publish(index, &done, TaskState::Completed, "视频已压缩并替换",
                    conversion.inputSizeBytes, conversion.outputSizeBytes);
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (unsigned int index = 0; index < workerCount; ++index) workers.emplace_back(worker);
    for (auto& thread : workers) thread.join();

    if (cancelRequested && cancelRequested->load()) wasCancelled.store(true);
    if (wasCancelled.load()) {
        result.cancelled = true;
        result.message = "压缩包处理已取消，原始文件保持不变。";
        return result;
    }
    if (failed.load()) {
        result.message = firstError.empty() ? "压缩包中的视频处理失败。" : firstError;
        return result;
    }

    if (result.processedVideos == 0 && result.skippedVideos == result.totalVideos) {
        result.success = true;
        result.skipped = true;
        result.message = "压缩包内所有视频都已使用相同参数处理，已自动跳过。";
        result.outputSizeBytes = result.inputSizeBytes;
        snapshot.fraction = 1.0;
        snapshot.status = "已跳过";
        snapshot.detail = result.message;
        if (onProgress) onProgress(snapshot);
        return result;
    }

    updateReferences(extracted, replacements, isPowerPoint);
    const fs::path manifestFile = extracted / pathFromUtf8(marker);
    fs::create_directories(manifestFile.parent_path(), fsError);
    {
        std::ofstream manifest(manifestFile, std::ios::binary | std::ios::trunc);
        const std::string content = manifestContent(input, settings);
        manifest.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!manifest) {
            result.message = "无法写入压缩包处理记录。";
            return result;
        }
    }

    const fs::path temporaryOutput = temporary.path() /
        pathFromUtf8("result" + pathToUtf8(output.extension()));
    if (!writeArchive(extracted, temporaryOutput, error)) {
        result.message = error;
        return result;
    }
    if (!validateArchive(temporaryOutput, isPowerPoint, error)) {
        result.message = "生成的压缩包未通过完整性检查：" + error;
        return result;
    }
    if (!replaceOutput(temporaryOutput, output, settings.overwrite, error)) {
        result.message = error;
        return result;
    }

    result.success = true;
    result.outputSizeBytes = fs::file_size(output, fsError);
    result.message = "压缩包处理完成，已更新 " + std::to_string(result.processedVideos) +
        " 个视频，跳过 " + std::to_string(result.skippedVideos) + " 个视频。";
    snapshot.fraction = 1.0;
    snapshot.status = "压缩包处理完成";
    snapshot.detail = result.message;
    if (onProgress) onProgress(snapshot);
    return result;
}

} // namespace ffmpegui
