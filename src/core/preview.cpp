#include "preview.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace ffmpegui {
namespace {
namespace fs = std::filesystem;

fs::path pathFromUtf8(std::string_view value) {
    std::u8string bytes;
    bytes.reserve(value.size());
    for (const unsigned char ch : value) bytes.push_back(static_cast<char8_t>(ch));
    return fs::path(bytes);
}

std::string entryPath(archive_entry* entry) {
    if (const char* utf8 = archive_entry_pathname_utf8(entry)) return utf8;
#ifdef _WIN32
    if (const wchar_t* wide = archive_entry_pathname_w(entry)) {
        const auto bytes = fs::path(wide).generic_u8string();
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
#endif
    if (const char* raw = archive_entry_pathname(entry)) return raw;
    return {};
}

struct PreviewDirectory {
    fs::path path;
    PreviewDirectory() {
        static std::atomic_uint64_t counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("ffmpegui-preview-" +
            std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
        fs::create_directories(path);
    }
    ~PreviewDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

bool extractMember(const std::string& archivePath, const std::string& member,
                   const fs::path& output, const std::atomic_bool& cancelled,
                   std::string& error) {
    archive* reader = archive_read_new();
    if (!reader) { error = "无法打开压缩包。"; return false; }
    archive_read_support_filter_all(reader);
    archive_read_support_format_zip(reader);
#ifdef _WIN32
    const int opened = archive_read_open_filename_w(
        reader, pathFromUtf8(archivePath).c_str(), 10240);
#else
    const int opened = archive_read_open_filename(reader, archivePath.c_str(), 10240);
#endif
    if (opened != ARCHIVE_OK) {
        error = "无法读取压缩包。";
        archive_read_free(reader);
        return false;
    }
    bool found = false;
    archive_entry* entry = nullptr;
    constexpr std::uint64_t limit = 12ULL * 1024ULL * 1024ULL * 1024ULL;
    while (!cancelled.load() && archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
        std::string name = entryPath(entry);
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name != member) {
            archive_read_data_skip(reader);
            continue;
        }
        if (archive_entry_filetype(entry) == AE_IFLNK ||
            archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
            !isVideoFile(name) || archive_entry_size(entry) < 0 ||
            static_cast<std::uint64_t>(archive_entry_size(entry)) > limit) {
            error = "压缩包内的视频文件无法安全提取。";
            break;
        }
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        if (!file) { error = "无法创建预览临时文件。"; break; }
        std::array<char, 128 * 1024> buffer{};
        std::uint64_t total = 0;
        la_ssize_t count = 0;
        while (!cancelled.load() &&
               (count = archive_read_data(reader, buffer.data(), buffer.size())) > 0) {
            total += static_cast<std::uint64_t>(count);
            if (total > limit) { error = "预览文件超过安全大小限制。"; break; }
            file.write(buffer.data(), count);
            if (!file) { error = "写入预览临时文件失败。"; break; }
        }
        if (count < 0) error = "读取压缩包内的视频失败。";
        found = error.empty() && !cancelled.load();
        break;
    }
    archive_read_close(reader);
    archive_read_free(reader);
    if (!found && error.empty() && !cancelled.load())
        error = "压缩包中找不到选中的视频。";
    return found;
}
} // namespace

AsyncVideoPreview::AsyncVideoPreview(FfmpegService service, std::string inputPath,
                                     std::string archiveMember, int maximumWidth, int maximumHeight)
    : state_(std::make_shared<State>(
          std::move(service), std::move(inputPath), std::move(archiveMember),
          maximumWidth, maximumHeight)) {
    std::thread([state = state_] { run(state); }).detach();
}

AsyncVideoPreview::~AsyncVideoPreview() {
    state_->cancelled.store(true);
    state_->changed.notify_all();
}

void AsyncVideoPreview::seek(double seconds) {
    {
        std::lock_guard lock(state_->mutex);
        state_->requestedSeconds = std::max(0.0, seconds);
        ++state_->sequence;
        state_->current.busy = true;
    }
    state_->changed.notify_one();
}

PreviewSnapshot AsyncVideoPreview::snapshot() const {
    std::lock_guard lock(state_->mutex);
    return state_->current;
}

void AsyncVideoPreview::run(const std::shared_ptr<State>& state) {
    try {
        std::string source = state->inputPath;
        std::unique_ptr<PreviewDirectory> temporary;
        if (!state->archiveMember.empty()) {
            temporary = std::make_unique<PreviewDirectory>();
            const fs::path entry = pathFromUtf8(state->archiveMember);
            const auto utf8 = (temporary->path / entry.filename()).u8string();
            source.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            std::string error;
            if (!extractMember(state->inputPath, state->archiveMember,
                               pathFromUtf8(source), state->cancelled, error)) {
                if (!state->cancelled.load()) {
                    std::lock_guard lock(state->mutex);
                    state->current.error = error;
                    state->current.busy = false;
                }
                return;
            }
        }
        if (state->cancelled.load()) return;
        const auto duration = state->service.probeDuration(source);
        if (!duration || *duration <= 0.0) {
            std::lock_guard lock(state->mutex);
            state->current.error = "无法读取视频时长，请检查 FFmpeg 与视频文件。";
            state->current.busy = false;
            return;
        }
        {
            std::lock_guard lock(state->mutex);
            state->current.duration = *duration;
        }
        std::uint64_t completed = 0;
        for (;;) {
            double at = 0.0;
            std::uint64_t sequence = 0;
            {
                std::unique_lock lock(state->mutex);
                state->changed.wait(lock, [&] {
                    return state->cancelled.load() || state->sequence != completed;
                });
                if (state->cancelled.load()) return;
                sequence = state->sequence;
                at = std::clamp(state->requestedSeconds, 0.0,
                                std::max(0.0, *duration - 0.05));
            }
            auto frame = state->service.previewFrame(source, at,
                state->maximumWidth, state->maximumHeight);
            {
                std::lock_guard lock(state->mutex);
                completed = sequence;
                if (state->cancelled.load()) return;
                if (sequence == state->sequence) {
                    state->current.frame = frame
                        ? std::make_shared<PreviewFrame>(std::move(*frame))
                        : nullptr;
                    state->current.frameSeconds = at;
                    state->current.busy = false;
                    state->current.error = frame ? "" :
                        "当前时间位置无法提取画面，可以向前跳转。";
                }
            }
        }
    } catch (const std::exception& exception) {
        if (state->cancelled.load()) return;
        std::lock_guard lock(state->mutex);
        state->current.error = exception.what();
        state->current.busy = false;
    }
}

} // namespace ffmpegui
