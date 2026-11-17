#pragma once

#include "archive.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace ffmpegui {

struct BatchItemSnapshot {
    std::string name;
    TaskState state{TaskState::Pending};
    double progress{0.0};
    ConversionProgress progressInfo;
    std::string detail;
    std::uint64_t inputSizeBytes{0};
    std::uint64_t outputSizeBytes{0};
};

struct BatchTaskSnapshot {
    std::uint64_t id{0};
    std::string inputPath;
    std::string outputPath;
    std::string relativeDirectory;
    TaskKind kind{TaskKind::Video};
    TaskState state{TaskState::Pending};
    double progress{0.0};
    std::string detail;
    std::string error;
    std::uint64_t inputSizeBytes{0};
    std::uint64_t outputSizeBytes{0};
    std::vector<BatchItemSnapshot> items;
};

struct AppSnapshot {
    BatchSettings settings;
    EncoderCapabilities capabilities;
    ResourcePlan resourcePlan;
    std::vector<BatchTaskSnapshot> tasks;
    bool running{false};
    bool paused{false};
    double overallProgress{0.0};
    std::size_t totalTasks{0};
    std::size_t pendingTasks{0};
    std::size_t runningTasks{0};
    std::size_t completedTasks{0};
    std::size_t skippedTasks{0};
    std::size_t failedTasks{0};
    std::string status{"就绪"};
    std::string detail;
    std::string error;
};

class AppController {
public:
    explicit AppController(FfmpegService service = FfmpegService());
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    void initialize();
    AppSnapshot snapshot() const;
    void updateSettings(const BatchSettings& settings);
    void addInputs(const std::vector<std::string>& paths);
    void removeTask(std::uint64_t id);
    void clearCompleted();
    void clearAll();
    std::size_t importQueue(const std::string& path, std::string& error);
    bool exportQueue(const std::string& path, std::string& error) const;
    bool startBatch();
    void pauseBatch();
    void resumeBatch();
    void cancelBatch();
    void shutdown();

private:
    void refreshOutputPathsLocked();
    std::string makeOutputPathLocked(const BatchTaskSnapshot& task) const;
    std::string makeUniqueOutputPathLocked(const BatchTaskSnapshot& task,
                                           const std::vector<std::string>& reserved) const;
    void loadSettings();
    void saveSettingsLocked() const;
    void runTask(std::size_t index, const ResourcePlan& plan, std::size_t batchTaskCount);

    FfmpegService service_;
    mutable std::mutex mutex_;
    BatchSettings settings_;
    EncoderCapabilities capabilities_;
    ResourcePlan resourcePlan_;
    std::vector<BatchTaskSnapshot> tasks_;
    bool running_{false};
    std::string status_{"就绪"};
    std::string detail_;
    std::string error_;
    std::atomic_bool cancelRequested_{false};
    std::atomic_bool pauseRequested_{false};
    std::thread worker_;
    std::uint64_t nextTaskId_{1};
};

} // namespace ffmpegui
