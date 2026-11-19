#include "app_controller.hpp"
#include "process.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <cstdlib>
#include <sstream>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <set>
#include <system_error>

namespace ffmpegui {
namespace {

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

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

TaskKind kindForPath(const std::string& path) {
    const std::string extension = lower(pathToUtf8(pathFromUtf8(path).extension()));
    if (extension == ".pptx") return TaskKind::PowerPoint;
    if (extension == ".zip") return TaskKind::ZipArchive;
    return TaskKind::Video;
}

std::string normalizedPath(const std::filesystem::path& path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    std::string value = pathToUtf8(error ? path.lexically_normal() : canonical);
#ifdef _WIN32
    value = lower(std::move(value));
#endif
    return value;
}

bool parseBool(const std::string& value, bool fallback) {
    if (value == "1" || value == "true" || value == "yes") return true;
    if (value == "0" || value == "false" || value == "no") return false;
    return fallback;
}

std::filesystem::path settingsFilePath() {
    const auto overrideDirectory = environmentVariable("FFMPEG_STUDIO_CONFIG_DIR");
    if (overrideDirectory && !overrideDirectory->empty()) {
        return pathFromUtf8(*overrideDirectory) / "settings.ini";
    }
#ifdef _WIN32
    std::wstring appData(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appData.data(), static_cast<DWORD>(appData.size()));
    if (length > 0 && length < appData.size()) {
        appData.resize(length);
        return pathFromUtf8(utf8FromWide(appData.c_str())) / "FFmpegStudio" / "settings.ini";
    }
    return std::filesystem::temp_directory_path() / "FFmpegStudio" / "settings.ini";
#else
    const auto xdg = environmentVariable("XDG_CONFIG_HOME");
    if (xdg && !xdg->empty()) {
        return pathFromUtf8(*xdg) / "ffmpeg-studio" / "settings.ini";
    }
    const auto home = environmentVariable("HOME");
    if (home && !home->empty()) {
        return pathFromUtf8(*home) / ".config" / "ffmpeg-studio" / "settings.ini";
    }
    return std::filesystem::temp_directory_path() / "ffmpeg-studio" / "settings.ini";
#endif
}

std::filesystem::path queueFilePath() {
    return settingsFilePath().parent_path() / "queue.ffq";
}

bool selectAvailableEncoder(BatchSettings& settings,
                            const EncoderCapabilities& capabilities) {
    if (capabilities.supports(settings.codec, settings.backend)) return true;
    const auto available = availableVideoCodecs(capabilities);
    if (available.empty()) return false;

    const auto chooseBackend = [&](VideoCodec codec) {
        for (const auto backend : {EncoderBackend::CPU, EncoderBackend::NVENC,
                                   EncoderBackend::QSV, EncoderBackend::AMF}) {
            if (!capabilities.supports(codec, backend)) continue;
            settings.backend = backend;
            return true;
        }
        return false;
    };
    if (!chooseBackend(settings.codec)) {
        // Preserve a WebM output choice when a different FFmpeg build is loaded.
        const std::string extension =
            outputVideoExtension(settings.outputContainer, "source.mp4");
        for (const auto candidate : available) {
            if (settings.outputContainer != OutputContainer::SameAsSource &&
                !containerSupportsVideoCodec(extension, candidate)) continue;
            settings.codec = candidate;
            break;
        }
        if (!chooseBackend(settings.codec)) {
            settings.codec = available.front();
            if (!chooseBackend(settings.codec)) return false;
        }
        settings.crf = defaultCrf(settings.codec);
    }
    if (settings.outputContainer != OutputContainer::SameAsSource &&
        !containerSupportsVideoCodec(
            outputVideoExtension(settings.outputContainer, "source.mp4"),
            settings.codec))
        settings.outputContainer = OutputContainer::MKV;
    return true;
}


} // namespace

AppController::AppController(FfmpegService service) : service_(std::move(service)) {
    settings_.crf = defaultCrf(settings_.codec);
    loadSettings();
    scanner_ = std::thread([this] { scanLoop(); });
    loadQueue();
}

AppController::~AppController() { shutdown(); }

void AppController::initialize() {
    std::lock_guard lock(mutex_);
    if (initializing_ || initializeWorker_.joinable()) return;
    initializing_ = true;
    status_ = "正在检测编码环境";
    detail_ = "正在后台检查 FFmpeg、编码器和可用设备，窗口可以正常操作。";
    initializeWorker_ = std::thread([this] { initializeNow(); });
}

void AppController::initializeNow() {
    EncoderCapabilities capabilities = service_.probeCapabilities(true);
    RemoteResources remoteResources;
    std::string remoteError;
    std::string selectedRemote;

    {
        std::lock_guard lock(mutex_);
        selectedRemote = settings_.remoteHost;
    }

    if (!selectedRemote.empty()) {
        const auto remote = service_.probeRemote(selectedRemote, true);
        if (remote.success) {
            capabilities = remote.capabilities;
            remoteResources = remote.resources;
        } else {
            remoteError = remote.error;
            selectedRemote.clear();
        }
    }

    std::lock_guard lock(mutex_);
    capabilities_ = capabilities;
    remoteResources_ = remoteResources;
    initializing_ = false;
    settings_.remoteHost = selectedRemote;

    (void)selectAvailableEncoder(settings_, capabilities_);

    saveSettingsLocked();

    if (!remoteError.empty()) {
        status_ = "远程服务器暂时不可用";
        error_ = "无法连接上次选择的 SSH 服务器，已切换到本机编码。" + remoteError;
    } else if (capabilities_.ffmpegVersion.empty()) {
        status_ = "未检测到 FFmpeg";
        error_ = "请安装 FFmpeg，并确保 ffmpeg 和 ffprobe 可以从命令行访问。";
    } else if (!capabilities_.ffprobeAvailable) {
        status_ = "缺少 ffprobe";
        error_ = "当前环境未检测到 ffprobe。请安装与 FFmpeg 配套的工具后重新启动程序。";
    } else {
        status_ = "就绪";
        detail_ = selectedRemote.empty()
            ? "本机 FFmpeg 与 ffprobe 已就绪。"
            : "已连接远程服务器 " + selectedRemote + "，后续任务将使用远程算力。";
        if (!tasks_.empty()) {
            detail_ += " 已恢复上次的 " + std::to_string(tasks_.size()) + " 个待处理任务。";
        }
        error_.clear();
    }
}

std::vector<SshServer> AppController::sshServers() const {
    return service_.sshService().listServers();
}

bool AppController::saveSshServer(const SshServer& server, const std::string& password,
                                  std::string& error) {
    std::lock_guard lock(mutex_);
    if (running_) {
        error = "批量任务运行期间不能修改 SSH 服务器。";
        return false;
    }

    return service_.sshService().saveServer(server, password, error);
}

bool AppController::removeSshServer(const std::string& alias, std::string& error) {
    {
        std::lock_guard lock(mutex_);
        if (running_) {
            error = "批量任务运行期间不能删除 SSH 服务器。";
            return false;
        }
    }

    const auto localCapabilities = service_.probeCapabilities(true);

    std::lock_guard lock(mutex_);
    if (running_) {
        error = "批量任务运行期间不能删除 SSH 服务器。";
        return false;
    }

    if (!service_.sshService().removeServer(alias, error)) {
        return false;
    }

    if (settings_.remoteHost == alias) {
        settings_.remoteHost.clear();
        remoteResources_ = {};
        capabilities_ = localCapabilities;

        if (!capabilities_.supports(settings_.codec, settings_.backend)) {
            for (const auto candidate : {
                     EncoderBackend::CPU,
                     EncoderBackend::NVENC,
                     EncoderBackend::QSV,
                     EncoderBackend::AMF
                 }) {
                if (capabilities_.supports(settings_.codec, candidate)) {
                    settings_.backend = candidate;
                    break;
                }
            }
        }

        refreshOutputPathsLocked();
        saveSettingsLocked();
        status_ = "使用本机编码";
        detail_ = "已删除当前远程服务器，后续任务将在本机使用 FFmpeg 处理。";
        error_.clear();
    }

    return true;
}

bool AppController::testSshServer(const std::string& alias, std::string& error) const {
    if (!service_.sshService().testConnection(alias, error)) {
        return false;
    }

    const auto remote = service_.probeRemote(alias, true);
    if (!remote.success) {
        error = remote.error;
        return false;
    }

    std::ostringstream summary;
    summary << "SSH 连接成功。远程系统：" << remote.resources.operatingSystem
            << "，逻辑 CPU：" << remote.resources.logicalCores;

    if (remote.resources.availableMemoryBytes > 0) {
        const auto availableMemoryMiB =
            remote.resources.availableMemoryBytes / (1024ULL * 1024ULL);
        summary << "，可用内存：" << availableMemoryMiB << " MiB";
    }

    summary << "。远程 FFmpeg 和编码器检测正常。";
    error = summary.str();
    return true;
}

bool AppController::selectRemoteServer(const std::string& alias, std::string& error) {
    {
        std::lock_guard lock(mutex_);
        if (running_) {
            error = "批量任务运行期间不能切换编码设备。";
            return false;
        }
    }

    const auto remote = service_.probeRemote(alias, true);
    if (!remote.success) {
        error = remote.error;
        return false;
    }

    std::lock_guard lock(mutex_);
    if (running_) {
        error = "批量任务运行期间不能切换编码设备。";
        return false;
    }

    if (!selectAvailableEncoder(settings_, remote.capabilities)) {
        error = "远程服务器没有可用于转换视频的编码器。";
        return false;
    }

    settings_.remoteHost = alias;
    capabilities_ = remote.capabilities;
    remoteResources_ = remote.resources;
    refreshOutputPathsLocked();
    saveSettingsLocked();

    status_ = "已选择远程服务器";
    detail_ = "后续任务将通过 SSH 使用 " + alias +
              " 上的 FFmpeg，处理结果会自动下载到本机。";
    error_.clear();
    error.clear();
    return true;
}

void AppController::selectLocalEncoder() {
    {
        std::lock_guard lock(mutex_);
        if (running_) {
            return;
        }
    }

    const auto localCapabilities = service_.probeCapabilities(true);

    std::lock_guard lock(mutex_);
    if (running_) {
        return;
    }

    settings_.remoteHost.clear();
    remoteResources_ = {};
    capabilities_ = localCapabilities;

    (void)selectAvailableEncoder(settings_, capabilities_);

    refreshOutputPathsLocked();
    saveSettingsLocked();
    status_ = "使用本机编码";
    detail_ = "视频将在本机使用 FFmpeg 处理。";
    error_.clear();
}

std::optional<double> AppController::videoDuration(const std::string& inputPath) const {
    return service_.probeDuration(inputPath);
}

std::optional<PreviewFrame> AppController::videoPreview(const std::string& inputPath,
                                                        double seconds, int width, int height) const {
    return service_.previewFrame(inputPath, seconds, width, height);
}

AppSnapshot AppController::snapshot() const {
    std::lock_guard lock(mutex_);
    AppSnapshot result;
    result.settings = settings_;
    result.capabilities = capabilities_;
    const bool singleArchive = tasks_.size() == 1 && tasks_.front().kind != TaskKind::Video;
    result.resourcePlan = running_ ? resourcePlan_ :
        (!tasks_.empty() ? recommendResourcePlan(singleArchive ? 64U : tasks_.size(), settings_.backend,
                                settings_.codec, settings_.maxParallelJobs,
                                remoteResources_.logicalCores, remoteResources_.availableMemoryBytes,
                                settings_.remoteHost.empty() ? "本机" : "远程服务器") : resourcePlan_);
    result.tasks = tasks_;
    result.running = running_;
    result.paused = pauseRequested_.load();
    result.initializing = initializing_;
    result.scanningInputs = scanningActive_ || !scanQueue_.empty();
    result.scannedEntries = scannedEntries_;
    result.discoveredVideos = discoveredVideos_;
    result.scanningPath = scanningPath_;
    result.status = status_;
    result.detail = detail_;
    result.error = error_;
    result.totalTasks = tasks_.size();

    double progressSum = 0.0;
    double totalWeight = 0.0;
    for (const auto& task : tasks_) {
        switch (task.state) {
        case TaskState::Pending: ++result.pendingTasks; break;
        case TaskState::Running: ++result.runningTasks; break;
        case TaskState::Completed: ++result.completedTasks; break;
        case TaskState::Skipped: ++result.skippedTasks; break;
        case TaskState::Failed: ++result.failedTasks; break;
        case TaskState::Cancelled: ++result.pendingTasks; break;
        }
        const double weight = task.kind == TaskKind::Video ? 1.0 :
            static_cast<double>(std::max<std::size_t>(1, task.items.size()));
        progressSum += task.progress * weight;
        totalWeight += weight;
    }
    result.overallProgress = totalWeight == 0.0 ? 0.0 : progressSum / totalWeight;
    return result;
}

void AppController::updateSettings(const BatchSettings& settings) {
    std::lock_guard lock(mutex_);
    if (running_) return;
    settings_ = settings;
    settings_.outputContainer = static_cast<OutputContainer>(
        std::clamp(static_cast<int>(settings_.outputContainer), 0, 3));
    settings_.codec = static_cast<VideoCodec>(
        std::clamp(static_cast<int>(settings_.codec), 0, 4));
    settings_.outputContainer = static_cast<OutputContainer>(
        std::clamp(static_cast<int>(settings_.outputContainer), 0, 3));
    settings_.codec = static_cast<VideoCodec>(
        std::clamp(static_cast<int>(settings_.codec), 0, 4));
    settings_.crf = std::clamp(settings_.crf, 0, crfMaximum(settings_.codec));
    settings_.audioBitrateKbps = std::clamp(settings_.audioBitrateKbps, 64, 320);
    settings_.audioMode = static_cast<AudioMode>(
        std::clamp(static_cast<int>(settings_.audioMode), 0, 2));
    settings_.outputResolution = static_cast<OutputResolution>(
        std::clamp(static_cast<int>(settings_.outputResolution), 0, 5));
    settings_.maxParallelJobs = std::clamp(settings_.maxParallelJobs, 0, 128);
    if (!validTrimRange(settings_.trimStartSeconds, settings_.trimEndSeconds)) {
        settings_.trimStartSeconds = 0.0;
        settings_.trimEndSeconds = -1.0;
    }
    refreshOutputPathsLocked();
    saveSettingsLocked();
}


void AppController::addInputs(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    {
        std::lock_guard lock(mutex_);
        if (running_ || stopScanning_.load()) return;
        for (const auto& path : paths) {
            if (!path.empty()) scanQueue_.push_back(path);
        }
        if (scanQueue_.empty()) return;
        status_ = "正在导入文件";
        detail_ = "正在后台读取视频与压缩包目录，发现的文件会逐步显示在队列中。";
        error_.clear();
    }
    scannerCv_.notify_one();
}

void AppController::scanLoop() {
    for (;;) {
        std::string path;
        {
            std::unique_lock lock(mutex_);
            scannerCv_.wait(lock, [this] {
                return stopScanning_.load() || !scanQueue_.empty();
            });
            if (stopScanning_.load()) return;
            path = std::move(scanQueue_.front());
            scanQueue_.pop_front();
            scanningPath_ = path;
            scanningActive_ = true;
            status_ = "正在扫描输入文件";
            detail_ = "正在检查：" + path;
        }
        try {
            scanInput(path);
        } catch (const std::exception& exception) {
            std::lock_guard lock(mutex_);
            error_ = "扫描失败：" + std::string(exception.what());
        }
        {
            std::lock_guard lock(mutex_);
            scanningActive_ = !scanQueue_.empty();
            if (!scanningActive_) {
                scanningPath_.clear();
                status_ = "文件导入完成";
                detail_ = "队列中有 " + std::to_string(tasks_.size()) +
                    " 个处理任务，已列出 " + std::to_string(discoveredVideos_) + " 个视频。";
            }
            saveQueueLocked();
        }
    }
}

void AppController::scanInput(const std::string& rawPath) {
    if (stopScanning_.load()) return;
    namespace fs = std::filesystem;
    const fs::path path = pathFromUtf8(rawPath);
    std::error_code error;
    auto addFile = [this, &rawPath](const fs::path& file, const fs::path& root) {
        if (stopScanning_.load()) return;
        const std::string fileUtf8 = pathToUtf8(file);
        if (!isSupportedInputFile(fileUtf8)) return;
        std::error_code fileError;
        if (!fs::is_regular_file(file, fileError) || fileError) return;
        const std::string normalized = normalizedPath(file);
        std::string relativeDirectory;
        if (!root.empty()) {
            const auto relative = fs::relative(file.parent_path(), root, fileError);
            if (!fileError && relative != ".") relativeDirectory = pathToUtf8(relative);
        }
        std::uint64_t id = 0;
        const TaskKind kind = kindForPath(fileUtf8);
        {
            std::lock_guard lock(mutex_);
            if (stopScanning_.load() || running_) return;
            const bool alreadyAdded = std::any_of(tasks_.begin(), tasks_.end(),
                [&](const BatchTaskSnapshot& current) {
                    return normalizedPath(pathFromUtf8(current.inputPath)) == normalized;
                });
            if (alreadyAdded) return;
            BatchTaskSnapshot task;
            task.id = nextTaskId_++;
            id = task.id;
            task.inputPath = normalized;
            task.relativeDirectory = relativeDirectory;
            task.kind = kind;
            task.detail = kind == TaskKind::Video ? "等待处理" : "正在读取内部视频列表…";
            std::vector<std::string> reserved;
            reserved.reserve(tasks_.size());
            for (const auto& current : tasks_)
                reserved.push_back(normalizedPath(pathFromUtf8(current.outputPath)));
            if (kind == TaskKind::Video) {
                BatchItemSnapshot item;
                item.name = pathToUtf8(file.filename());
                item.detail = "等待处理";
                if (const auto saved = restoredTrims_.find(normalized);
                    saved != restoredTrims_.end()) {
                    if (const auto range = saved->second.find("");
                        range != saved->second.end()) item.trim = range->second;
                }
                task.items.push_back(std::move(item));
                ++discoveredVideos_;
            }
            task.outputPath = makeUniqueOutputPathLocked(task, reserved);
            tasks_.push_back(std::move(task));
            ++scannedEntries_;
            detail_ = "已发现 " + std::to_string(discoveredVideos_) + " 个视频，正在读取：" + rawPath;
        }
        if (kind == TaskKind::Video) return;
        std::string inspectError;
        const bool inspected = inspectArchiveVideos(normalized,
            [this, id, &normalized](const std::string& name) {
                std::lock_guard lock(mutex_);
                if (stopScanning_.load()) return;
                const auto task = std::find_if(tasks_.begin(), tasks_.end(),
                    [id](const BatchTaskSnapshot& value) { return value.id == id; });
                if (task == tasks_.end()) return;
                if (std::any_of(task->items.begin(), task->items.end(),
                    [&name](const BatchItemSnapshot& value) { return value.name == name; }))
                    return;
                BatchItemSnapshot item;
                item.name = name;
                item.detail = "等待处理";
                if (const auto saved = restoredTrims_.find(normalized);
                    saved != restoredTrims_.end()) {
                    if (const auto range = saved->second.find(name);
                        range != saved->second.end()) item.trim = range->second;
                }
                task->items.push_back(std::move(item));
                ++discoveredVideos_;
                task->detail = "已发现 " + std::to_string(task->items.size()) + " 个视频，正在扫描…";
                detail_ = "已发现 " + std::to_string(discoveredVideos_) + " 个视频，正在读取：" + scanningPath_;
            }, inspectError, &stopScanning_);
        {
            std::lock_guard lock(mutex_);
            const auto task = std::find_if(tasks_.begin(), tasks_.end(),
                [id](const BatchTaskSnapshot& value) { return value.id == id; });
            if (task == tasks_.end()) return;
            if (!inspected && !stopScanning_.load()) {
                task->detail = inspectError;
                task->error = inspectError;
                task->state = TaskState::Failed;
            } else {
                task->detail = task->items.empty()
                    ? "未找到可处理的视频"
                    : "已列出 " + std::to_string(task->items.size()) + " 个视频，等待处理";
            }
            refreshOutputPathsLocked();
        }
    };
    if (fs::is_directory(path, error)) {
        const fs::path root = fs::weakly_canonical(path, error);
        if (error) return;
        bool recursive = true;
        {
            std::lock_guard lock(mutex_);
            recursive = settings_.recursive;
        }
        if (recursive) {
            for (fs::recursive_directory_iterator it(root,
                     fs::directory_options::skip_permission_denied, error), end;
                 it != end && !stopScanning_.load();) {
                std::error_code entryError;
                if (it->is_symlink(entryError)) {
                    if (it->is_directory(entryError)) it.disable_recursion_pending();
                } else if (it->is_regular_file(entryError)) addFile(it->path(), root);
                it.increment(error);
                if (error) error.clear();
            }
        } else {
            for (fs::directory_iterator it(root,
                     fs::directory_options::skip_permission_denied, error), end;
                 it != end && !stopScanning_.load();) {
                std::error_code entryError;
                if (it->is_regular_file(entryError)) addFile(it->path(), root);
                it.increment(error);
                if (error) error.clear();
            }
        }
    } else if (fs::is_regular_file(path, error)) {
        addFile(path, {});
    }
}

bool AppController::setVideoTrim(std::uint64_t taskId, const std::string& videoName,
                                 const VideoTrimRange& trim) {
    if (!validTrimRange(trim.startSeconds, trim.endSeconds)) return false;
    std::lock_guard lock(mutex_);
    if (running_) return false;
    const auto task = std::find_if(tasks_.begin(), tasks_.end(),
        [taskId](const BatchTaskSnapshot& value) { return value.id == taskId; });
    if (task == tasks_.end()) return false;
    if (task->kind == TaskKind::Video && !videoName.empty()) return false;
    if (task->kind != TaskKind::Video && videoName.empty()) return false;
    auto item = std::find_if(task->items.begin(), task->items.end(),
        [&](const BatchItemSnapshot& value) {
            return task->kind == TaskKind::Video || value.name == videoName;
        });
    if (item == task->items.end()) return false;
    item->trim = trim;
    item->state = TaskState::Pending;
    item->progress = 0.0;
    item->detail = "等待处理";
    task->state = TaskState::Pending;
    task->progress = 0.0;
    task->detail = "等待处理";
    task->error.clear();
    refreshOutputPathsLocked();
    saveQueueLocked();
    return true;
}

void AppController::removeTask(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    if (running_) return;
    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(), [id](const auto& task) {
        return task.id == id;
    }), tasks_.end());
    saveQueueLocked();
}

void AppController::clearCompleted() {
    std::lock_guard lock(mutex_);
    if (running_) return;
    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(), [](const auto& task) {
        return task.state == TaskState::Completed || task.state == TaskState::Skipped;
    }), tasks_.end());
    saveQueueLocked();
}

void AppController::clearAll() {
    std::lock_guard lock(mutex_);
    if (running_) return;
    tasks_.clear();
    saveQueueLocked();
    status_ = "就绪";
    detail_ = "任务列表已清空。";
    error_.clear();
}

void AppController::loadSettings() {
    std::ifstream input(settingsFilePath(), std::ios::binary);
    if (!input) return;
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        try {
            if (key == "outputContainer") {
                settings_.outputContainer = static_cast<OutputContainer>(
                    std::clamp(std::stoi(value), 0, 3));
            } else if (key == "codec") {
                settings_.codec = static_cast<VideoCodec>(
                    std::clamp(std::stoi(value), 0, 4));
            } else if (key == "backend") {
                settings_.backend = static_cast<EncoderBackend>(
                    std::clamp(std::stoi(value), 0, 3));
            } else if (key == "compression") {
                settings_.compression = static_cast<CompressionLevel>(
                    std::clamp(std::stoi(value), 0, 7));
            } else if (key == "crf") {
                settings_.crf = std::stoi(value);
            } else if (key == "audioBitrateKbps") {
                settings_.audioBitrateKbps = std::stoi(value);
            } else if (key == "audioMode") {
                settings_.audioMode = static_cast<AudioMode>(
                    std::clamp(std::stoi(value), 0, 2));
            } else if (key == "outputResolution") {
                settings_.outputResolution = static_cast<OutputResolution>(
                    std::clamp(std::stoi(value), 0, 5));
            } else if (key == "trimStartSeconds") {
                settings_.trimStartSeconds = std::stod(value);
            } else if (key == "trimEndSeconds") {
                settings_.trimEndSeconds = std::stod(value);
            } else if (key == "maxParallelJobs") {
                settings_.maxParallelJobs = std::stoi(value);
            } else if (key == "recursive") {
                settings_.recursive = parseBool(value, settings_.recursive);
            } else if (key == "preserveDirectoryStructure") {
                settings_.preserveDirectoryStructure =
                    parseBool(value, settings_.preserveDirectoryStructure);
            } else if (key == "skipAlreadyProcessed") {
                settings_.skipAlreadyProcessed =
                    parseBool(value, settings_.skipAlreadyProcessed);
            } else if (key == "overwrite") {
                settings_.overwrite = parseBool(value, settings_.overwrite);
            } else if (key == "outputDirectory") {
                settings_.outputDirectory = value;
            } else if (key == "remoteHost") {
                settings_.remoteHost = value;
            }
        } catch (const std::exception&) {
            // Invalid persisted values are ignored; the in-memory defaults remain active.
        }
    }
    settings_.crf = std::clamp(settings_.crf, 0, crfMaximum(settings_.codec));
    settings_.audioBitrateKbps = std::clamp(settings_.audioBitrateKbps, 64, 320);
    settings_.audioMode = static_cast<AudioMode>(
        std::clamp(static_cast<int>(settings_.audioMode), 0, 2));
    settings_.outputResolution = static_cast<OutputResolution>(
        std::clamp(static_cast<int>(settings_.outputResolution), 0, 5));
    settings_.maxParallelJobs = std::clamp(settings_.maxParallelJobs, 0, 128);
    if (!validTrimRange(settings_.trimStartSeconds, settings_.trimEndSeconds)) {
        settings_.trimStartSeconds = 0.0;
        settings_.trimEndSeconds = -1.0;
    }
}

void AppController::loadQueue() {
    std::ifstream input(queueFilePath(), std::ios::binary);
    if (!input) return;
    std::vector<std::string> paths;
    std::string line;
    bool version2 = false;
    std::string currentPath;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "FFmpegStudioQueue/2") { version2 = true; continue; }
        if (line == "FFmpegStudioQueue/1" || line.empty()) continue;
        if (version2) {
            std::istringstream record(line);
            char kind = 0;
            record >> kind;
            if (kind == 'T') {
                std::string name;
                if (record >> std::quoted(name)) {
                    currentPath = name;
                    paths.push_back(name);
                }
            } else if (kind == 'I' && !currentPath.empty()) {
                std::string name;
                VideoTrimRange trim;
                if ((record >> std::quoted(name) >> trim.startSeconds >> trim.endSeconds) &&
                    validTrimRange(trim.startSeconds, trim.endSeconds))
                    restoredTrims_[normalizedPath(pathFromUtf8(currentPath))][name] = trim;
            }
        } else paths.push_back(line);
    }
    if (!paths.empty()) addInputs(paths);
}

void AppController::saveQueueLocked() const {
    const auto path = queueFilePath();
    auto temporaryPath = path;
    temporaryPath += ".tmp";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return;
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output) return;
        output << "FFmpegStudioQueue/2\n";
        for (const auto& task : tasks_) {
            if (task.state == TaskState::Completed || task.state == TaskState::Skipped)
                continue;
            output << "T " << std::quoted(task.inputPath) << '\n';
            for (const auto& item : task.items) {
                const auto& trim = item.trim;
                if (trim.startSeconds == 0.0 && trim.endSeconds < 0.0) continue;
                const std::string name = task.kind == TaskKind::Video ? "" : item.name;
                output << "I " << std::quoted(name) << ' '
                       << trim.startSeconds << ' ' << trim.endSeconds << '\n';
            }
        }
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporaryPath, error);
            return;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(
            temporaryPath.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporaryPath, error);
    }
#else
    std::filesystem::rename(temporaryPath, path, error);
    if (error) std::filesystem::remove(temporaryPath, error);
#endif
}

void AppController::saveSettingsLocked() const {
    const auto path = settingsFilePath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << "outputContainer=" << static_cast<int>(settings_.outputContainer) << '\n'
           << "codec=" << static_cast<int>(settings_.codec) << '\n'
           << "backend=" << static_cast<int>(settings_.backend) << '\n'
           << "compression=" << static_cast<int>(settings_.compression) << '\n'
           << "crf=" << settings_.crf << '\n'
           << "audioBitrateKbps=" << settings_.audioBitrateKbps << '\n'
           << "audioMode=" << static_cast<int>(settings_.audioMode) << '\n'
           << "outputResolution=" << static_cast<int>(settings_.outputResolution) << '\n'
           << "trimStartSeconds=" << std::to_string(settings_.trimStartSeconds) << '\n'
           << "trimEndSeconds=" << std::to_string(settings_.trimEndSeconds) << '\n'
           << "maxParallelJobs=" << settings_.maxParallelJobs << '\n'
           << "recursive=" << (settings_.recursive ? 1 : 0) << '\n'
           << "preserveDirectoryStructure=" << (settings_.preserveDirectoryStructure ? 1 : 0) << '\n'
           << "skipAlreadyProcessed=" << (settings_.skipAlreadyProcessed ? 1 : 0) << '\n'
           << "overwrite=" << (settings_.overwrite ? 1 : 0) << '\n'
           << "outputDirectory=" << settings_.outputDirectory << '\n'
           << "remoteHost=" << settings_.remoteHost << '\n';
}


std::size_t AppController::importQueue(const std::string& path, std::string& error) {
    std::ifstream input(pathFromUtf8(path), std::ios::binary);
    if (!input) { error = "无法读取任务清单。"; return 0; }
    std::vector<std::string> paths;
    std::map<std::string, VideoTrimRanges> loadedTrims;
    std::string line, current;
    bool version2 = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "FFmpegStudioQueue/2") { version2 = true; continue; }
        if (line == "FFmpegStudioQueue/1" || line.empty()) continue;
        if (version2) {
            std::istringstream record(line);
            char type = 0;
            record >> type;
            if (type == 'T') {
                std::string name;
                if (record >> std::quoted(name)) {
                    current = name;
                    paths.push_back(name);
                }
            } else if (type == 'I' && !current.empty()) {
                std::string name;
                VideoTrimRange trim;
                if ((record >> std::quoted(name) >> trim.startSeconds >> trim.endSeconds) &&
                    validTrimRange(trim.startSeconds, trim.endSeconds))
                    loadedTrims[normalizedPath(pathFromUtf8(current))][name] = trim;
            }
        } else paths.push_back(line);
    }
    {
        std::lock_guard lock(mutex_);
        for (auto& [name, ranges] : loadedTrims) {
            for (const auto& [video, trim] : ranges) restoredTrims_[name][video] = trim;
        }
    }
    addInputs(paths);
    error.clear();
    return paths.size();
}

bool AppController::exportQueue(const std::string& path, std::string& error) const {
    std::ofstream output(pathFromUtf8(path), std::ios::binary | std::ios::trunc);
    if (!output) { error = "无法保存任务清单。"; return false; }
    std::lock_guard lock(mutex_);
    output << "FFmpegStudioQueue/2\n";
    for (const auto& task : tasks_) {
        output << "T " << std::quoted(task.inputPath) << '\n';
        for (const auto& item : task.items) {
            const auto& trim = item.trim;
            if (trim.startSeconds == 0.0 && trim.endSeconds < 0.0) continue;
            const std::string name = task.kind == TaskKind::Video ? "" : item.name;
            output << "I " << std::quoted(name) << ' '
                   << trim.startSeconds << ' ' << trim.endSeconds << '\n';
        }
    }
    if (!output) { error = "写入任务清单时发生错误。"; return false; }
    error.clear();
    return true;
}

std::string AppController::makeOutputPathLocked(const BatchTaskSnapshot& task) const {
    ConversionSettings encoding;
    encoding.outputContainer = settings_.outputContainer;
    encoding.codec = settings_.codec;
    encoding.backend = settings_.backend;
    encoding.compression = settings_.compression;
    encoding.crf = settings_.crf;
    encoding.audioBitrateKbps = settings_.audioBitrateKbps;
    encoding.audioMode = settings_.audioMode;
    encoding.outputResolution = settings_.outputResolution;
    if (task.kind == TaskKind::Video && !task.items.empty()) {
        encoding.trimStartSeconds = task.items.front().trim.startSeconds;
        encoding.trimEndSeconds = task.items.front().trim.endSeconds;
    }
    std::string suffix = outputSuffix(encoding);
    if (task.kind != TaskKind::Video) {
        std::uint64_t fingerprint = 14695981039346656037ULL;
        bool hasTrim = false;
        for (const auto& item : task.items) {
            if (item.trim.startSeconds == 0.0 && item.trim.endSeconds < 0.0) continue;
            hasTrim = true;
            const std::string key = item.name + ":" +
                std::to_string(std::llround(item.trim.startSeconds * 1000.0)) + ":" +
                std::to_string(std::llround(item.trim.endSeconds * 1000.0));
            for (const unsigned char c : key) {
                fingerprint ^= c;
                fingerprint *= 1099511628211ULL;
            }
        }
        if (hasTrim) {
            std::ostringstream hex;
            hex << std::hex << fingerprint;
            suffix += "_T" + hex.str();
        }
    }
    const std::filesystem::path input = pathFromUtf8(task.inputPath);
    std::filesystem::path directory = input.parent_path();
    if (!settings_.outputDirectory.empty()) {
        directory = pathFromUtf8(settings_.outputDirectory);
        if (settings_.preserveDirectoryStructure && !task.relativeDirectory.empty())
            directory /= pathFromUtf8(task.relativeDirectory);
    }
    const std::string extension = task.kind == TaskKind::PowerPoint ? ".pptx" :
                                  task.kind == TaskKind::ZipArchive ? ".zip" :
                                  outputVideoExtension(settings_.outputContainer, task.inputPath);
    const std::string fileName = pathToUtf8(input.stem()) + suffix + extension;
    return pathToUtf8(directory / pathFromUtf8(fileName));
}

std::string AppController::makeUniqueOutputPathLocked(const BatchTaskSnapshot& task,
                                                       const std::vector<std::string>& reserved) const {
    std::filesystem::path candidate = pathFromUtf8(makeOutputPathLocked(task));
    auto isReserved = [&](const std::filesystem::path& path) {
        const std::string key = normalizedPath(path);
        return std::any_of(reserved.begin(), reserved.end(), [&](const std::string& value) {
            return value == key;
        });
    };
    if (!isReserved(candidate)) return pathToUtf8(candidate);

    const std::string stem = pathToUtf8(candidate.stem());
    const std::string extension = pathToUtf8(candidate.extension());
    unsigned int duplicate = 1;
    do {
        ++duplicate;
        candidate = candidate.parent_path() / pathFromUtf8(stem + "_" + std::to_string(duplicate) + extension);
    } while (isReserved(candidate));
    return pathToUtf8(candidate);
}

void AppController::refreshOutputPathsLocked() {
    std::vector<std::string> reserved;
    reserved.reserve(tasks_.size());
    for (auto& task : tasks_) {
        task.outputPath = makeUniqueOutputPathLocked(task, reserved);
        reserved.push_back(normalizedPath(pathFromUtf8(task.outputPath)));
    }
}

bool AppController::startBatch() {
    {
        std::lock_guard lock(mutex_);
        if (initializing_ || scanningActive_ || !scanQueue_.empty()) {
            status_ = "正在准备任务";
            detail_ = "请在后台检测和视频列表读取完成后开始批量处理。";
            return false;
        }
    }
    if (worker_.joinable()) worker_.join();
    std::vector<std::size_t> pending;
    ResourcePlan plan;
    {
        std::lock_guard lock(mutex_);
        if (running_) return false;
        for (std::size_t index = 0; index < tasks_.size(); ++index) {
            auto& task = tasks_[index];
            if (task.state == TaskState::Pending || task.state == TaskState::Failed ||
                task.state == TaskState::Cancelled) {
                task.state = TaskState::Pending;
                task.progress = 0.0;
                task.error.clear();
                task.detail = "等待处理";
                pending.push_back(index);
            }
        }
        if (pending.empty()) {
            status_ = "没有待处理任务";
            detail_ = "请先添加文件或文件夹。";
            return false;
        }
        if (!capabilities_.ffprobeAvailable) {
            status_ = "FFmpeg 环境不可用";
            error_ = "请安装 FFmpeg 和 ffprobe 后重新启动程序。";
            return false;
        }
        if (!capabilities_.supports(settings_.codec, settings_.backend)) {
            status_ = "编码器不可用";
            error_ = "当前设备不支持所选编码器，请选择其他编码器。";
            return false;
        }
        const bool singleArchive = pending.size() == 1 &&
            tasks_[pending.front()].kind != TaskKind::Video;
        plan = recommendResourcePlan(singleArchive ? 64U : pending.size(), settings_.backend,
                                     settings_.codec, settings_.maxParallelJobs,
                                     remoteResources_.logicalCores, remoteResources_.availableMemoryBytes,
                                     settings_.remoteHost.empty() ? "本机" : "远程服务器");
        resourcePlan_ = plan;
        running_ = true;
        cancelRequested_.store(false);
        pauseRequested_.store(false);
        status_ = "正在处理批量任务";
        detail_ = plan.explanation;
        error_.clear();
    }

    worker_ = std::thread([this, pending = std::move(pending), plan]() {
        std::atomic_size_t next{0};
        const std::size_t batchCount = pending.size();
        const unsigned int workerCount = std::max(1U, std::min<unsigned int>(
            plan.parallelJobs, static_cast<unsigned int>(batchCount)));
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (unsigned int workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
            workers.emplace_back([&, workerIndex]() {
                (void)workerIndex;
                while (!cancelRequested_.load()) {
                    while (pauseRequested_.load() && !cancelRequested_.load())
                        std::this_thread::sleep_for(std::chrono::milliseconds(40));
                    if (cancelRequested_.load()) break;
                    const std::size_t queueIndex = next.fetch_add(1);
                    if (queueIndex >= pending.size()) break;
                    runTask(pending[queueIndex], plan, batchCount);
                }
            });
        }
        for (auto& thread : workers) thread.join();
        std::lock_guard lock(mutex_);
        running_ = false;
        pauseRequested_.store(false);
        if (cancelRequested_.load()) {
            status_ = "批量处理已取消";
            detail_ = "已停止当前编码任务，未完成的文件保留在等待列表中。";
        } else {
            const auto completed = std::count_if(tasks_.begin(), tasks_.end(), [](const auto& task) {
                return task.state == TaskState::Completed;
            });
            const auto skipped = std::count_if(tasks_.begin(), tasks_.end(), [](const auto& task) {
                return task.state == TaskState::Skipped;
            });
            const auto failed = std::count_if(tasks_.begin(), tasks_.end(), [](const auto& task) {
                return task.state == TaskState::Failed;
            });
            status_ = failed == 0 ? "批量处理完成" : "批量处理结束，部分任务失败";
            detail_ = "完成 " + std::to_string(completed) + " 个，跳过 " +
                      std::to_string(skipped) + " 个，失败 " + std::to_string(failed) + " 个。";
        }
    });
    return true;
}

void AppController::runTask(std::size_t index, const ResourcePlan& plan, std::size_t batchTaskCount) {
    BatchTaskSnapshot task;
    BatchSettings batchSettings;
    EncoderCapabilities capabilities;
    {
        std::lock_guard lock(mutex_);
        if (index >= tasks_.size()) return;
        auto& current = tasks_[index];
        current.state = TaskState::Running;
        current.progress = 0.0;
        current.detail = "正在准备处理";
        task = current;
        batchSettings = settings_;
        capabilities = capabilities_;
    }

    ConversionSettings settings;
    settings.inputPath = task.inputPath;
    settings.outputPath = task.outputPath;
    settings.remoteHost = batchSettings.remoteHost;
    settings.outputContainer = batchSettings.outputContainer;
    settings.codec = batchSettings.codec;
    settings.backend = batchSettings.backend;
    settings.compression = batchSettings.compression;
    settings.crf = batchSettings.crf;
    settings.audioBitrateKbps = batchSettings.audioBitrateKbps;
    settings.audioMode = batchSettings.audioMode;
    settings.outputResolution = batchSettings.outputResolution;
    settings.trimStartSeconds = 0.0;
    settings.trimEndSeconds = -1.0;
    if (task.kind == TaskKind::Video && !task.items.empty()) {
        settings.trimStartSeconds = task.items.front().trim.startSeconds;
        settings.trimEndSeconds = task.items.front().trim.endSeconds;
    }
    settings.threadsPerJob = static_cast<int>(plan.threadsPerJob);
    settings.overwrite = batchSettings.overwrite;
    settings.skipAlreadyProcessed = batchSettings.skipAlreadyProcessed;

    auto updateVideoProgress = [this, index](const ConversionProgress& progress) {
        std::lock_guard lock(mutex_);
        if (index >= tasks_.size()) return;
        auto& task = tasks_[index];
        task.progress = progress.fraction;
        task.detail = progress.detail;
        if (!task.items.empty()) {
            task.items[0].progress = progress.fraction;
            task.items[0].progressInfo = progress;
            task.items[0].detail = progress.detail;
            task.items[0].state = TaskState::Running;
        }
    };

    if (task.kind == TaskKind::Video) {
        const auto result = service_.convert(settings, capabilities, updateVideoProgress,
                                             &cancelRequested_, &pauseRequested_);
        std::lock_guard lock(mutex_);
        auto& current = tasks_[index];
        current.inputSizeBytes = result.inputSizeBytes;
        current.outputSizeBytes = result.outputSizeBytes;
        if (result.cancelled) {
            current.state = TaskState::Cancelled;
            current.detail = result.message;
            if (!current.items.empty()) current.items[0].state = TaskState::Cancelled;
        } else if (!result.success) {
            current.state = TaskState::Failed;
            current.error = result.message;
            current.detail = result.message;
            if (!current.items.empty()) {
                current.items[0].state = TaskState::Failed;
                current.items[0].detail = result.message;
            }
        } else {
            current.state = result.skipped ? TaskState::Skipped : TaskState::Completed;
            current.progress = 1.0;
            current.detail = result.message;
            if (!current.items.empty()) {
                current.items[0].state = result.skipped ? TaskState::Skipped : TaskState::Completed;
                current.items[0].progress = 1.0;
                current.items[0].detail = result.message;
            }
        }
        saveQueueLocked();
        return;
    }

    VideoTrimRanges perVideoTrims;
    for (const auto& item : task.items)
        perVideoTrims[item.name] = item.trim;
    ArchiveService archive(service_);
    const unsigned int archiveParallel = batchTaskCount == 1 ? plan.parallelJobs : 1U;
    const auto result = archive.process(task.inputPath, task.outputPath,
        task.kind == TaskKind::PowerPoint, settings, capabilities,
        archiveParallel, plan.threadsPerJob,
        [this, index](const ArchiveProgressSnapshot& progress) {
            std::lock_guard lock(mutex_);
            if (index >= tasks_.size()) return;
            auto& task = tasks_[index];
            task.progress = progress.fraction;
            task.detail = progress.detail;
            const auto previousItems = std::move(task.items);
            task.items.clear();
            task.items.reserve(progress.items.size());
            for (const auto& item : progress.items) {
                BatchItemSnapshot snapshot;
                snapshot.name = item.name;
                if (const auto previous = std::find_if(previousItems.begin(), previousItems.end(),
                    [&](const BatchItemSnapshot& value) { return value.name == item.name; });
                    previous != previousItems.end()) snapshot.trim = previous->trim;
                snapshot.state = item.state;
                snapshot.progress = item.progress.fraction;
                snapshot.progressInfo = item.progress;
                snapshot.detail = item.detail;
                snapshot.inputSizeBytes = item.inputSizeBytes;
                snapshot.outputSizeBytes = item.outputSizeBytes;
                task.items.push_back(std::move(snapshot));
            }
        }, &cancelRequested_, &pauseRequested_, perVideoTrims);

    std::lock_guard lock(mutex_);
    auto& current = tasks_[index];
    current.inputSizeBytes = result.inputSizeBytes;
    current.outputSizeBytes = result.outputSizeBytes;
    if (result.cancelled) {
        current.state = TaskState::Cancelled;
        current.detail = result.message;
    } else if (!result.success) {
        current.state = TaskState::Failed;
        current.error = result.message;
        current.detail = result.message;
    } else {
        current.state = result.skipped ? TaskState::Skipped : TaskState::Completed;
        current.progress = 1.0;
        current.detail = result.message;
    }
    saveQueueLocked();
}

void AppController::pauseBatch() {
    std::lock_guard lock(mutex_);
    if (!running_ || cancelRequested_.load() || pauseRequested_.exchange(true)) return;
    status_ = "正在暂停任务";
    detail_ = "当前 FFmpeg 进程已暂停，任务队列会在继续处理后恢复。";
}

void AppController::resumeBatch() {
    std::lock_guard lock(mutex_);
    if (!running_ || !pauseRequested_.exchange(false)) return;
    status_ = "正在处理批量任务";
    detail_ = resourcePlan_.explanation;
}

void AppController::cancelBatch() {
    pauseRequested_.store(false);
    if (cancelRequested_.exchange(true)) return;
    std::lock_guard lock(mutex_);
    if (running_) {
        status_ = "正在取消任务";
        detail_ = "正在停止 FFmpeg 进程并清理临时文件。";
    }
}

void AppController::shutdown() {
    cancelRequested_.store(true);
    stopScanning_.store(true);
    scannerCv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (scanner_.joinable()) scanner_.join();
    if (initializeWorker_.joinable()) initializeWorker_.join();
}

} // namespace ffmpegui
