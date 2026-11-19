# 项目维护台账

本文记录已经完成的重要维护事项、对应代码区域和可核对的验证结果。请把新的重要功能或发布记录按日期补充到本文，并同时更新相关技术说明。项目使用方法见 [README](../README.md)，接管顺序见[接管与维护手册](handover.md)。

## 当前状态（2026-11-19）

| 项目 | 当前记录 |
|---|---|
| 主分支 | `main` |
| 代码所声明的项目版本 | `1.2.7`，来自 `CMakeLists.txt` |
| 已确认的主分支功能提交 | [`76217c941cc0`](https://github.com/HSOS6Miyabi/FFMPEG-UI/commit/76217c941cc0063a786ce6bf524d3d7711c1722f) |
| 主分支最近一次已确认成功的完整 CI | [运行 44469229573](https://github.com/HSOS6Miyabi/FFMPEG-UI/actions/runs/44469229573)，Windows、Linux 和发布作业均成功 |
| 已发布的版本标签 | [`v1.2.7`](https://github.com/HSOS6Miyabi/FFMPEG-UI/releases/tag/v1.2.7) |
| `v1.2.7` 的原始发布提交 | [`8ccf49d915a3`](https://github.com/HSOS6Miyabi/FFMPEG-UI/commit/8ccf49d915a3723052e338a1fef63d7e264ac1c5) |
| 下一次正式发布 | 先更新 `CMakeLists.txt` 的版本，再执行完整的 Windows 和 Linux CI；已有标签对应的发布包不会因主分支后续提交而自动替换 |

版本号、Release 标签、源代码提交分别记录，可以防止交接时误以为已发布的安装包包含后续主分支修改。

## 已完成的重要工作

### 2026-11-19：独立视频裁剪与后台扫描

- 已在 `AppController` 的每个 `BatchItemSnapshot` 中保存独立 `VideoTrimRange`。独立视频使用其单个任务项目；ZIP/PPTX 内部视频以相对路径区分。
- 目录遍历和 ZIP/PPTX 内部文件名读取由后台扫描线程执行，队列逐步显示找到的视频。编码器检测也使用后台线程。
- Windows 与 Linux 的裁剪器显示视频画面以及可拖动的起点、终点和预览游标。时长探测、画面提取和归档内部视频临时提取由 `AsyncVideoPreview` 在后台执行。
- 自动恢复清单及手动导出的 `.ffq` 采用版本 2 格式保存各视频裁剪时间，仍能读取版本 1 的路径清单。
- ZIP/PPTX 处理时，每个内部视频使用自己保存的裁剪起止时间；普通视频和 SSH 视频处理也使用相应任务的设置。
- 修改涉及 `src/core/app_controller.*`、`archive.*`、`preview.*`、`conversion.hpp`、两套原生界面及自动化测试。
- 最近一次已确认通过的完整功能验证为 [运行 44469229573](https://github.com/HSOS6Miyabi/FFMPEG-UI/actions/runs/44469229573)。该次执行包括 Windows/Linux 编译、单元测试、真实视频及 ZIP/PPTX 测试、Linux SSH 测试和双平台界面截图。
- 对应最终功能提交为 [`76217c941cc0`](https://github.com/HSOS6Miyabi/FFMPEG-UI/commit/76217c941cc0063a786ce6bf524d3d7711c1722f)。

### 2026-11-19：持续集成环境维护

- Linux CI 使用 `ubuntu-24.04`；依赖安装时设置网络请求超时，并提供重复尝试。Windows CI 使用 `windows-latest`。
- Windows CI 验证 NSIS 安装、卸载、FFmpeg 随包提供、媒体处理及真实桌面界面；Linux CI 验证 X11/Xft 界面、DPI 缩放、裁剪交互及临时 SSH 服务器。
- [运行 44469229573](https://github.com/HSOS6Miyabi/FFMPEG-UI/actions/runs/44469229573) 的 Linux、Windows、Release 作业均完成且结果为成功。

### 2026-11-18：可视化裁剪与 1.2.7 发布

- 实现 Windows Win32/GDI 与 Linux X11/Xft 的视频裁剪对话框、时间输入和画面预览，FFmpeg 使用输入侧 `-ss` 和输出侧 `-t`。
- 改进 DPI、紧凑窗口和队列显示，完善 VP9 极致压缩预设，并检查 Windows 卸载时的快捷方式处理。
- 已从提交 [`8ccf49d915a3`](https://github.com/HSOS6Miyabi/FFMPEG-UI/commit/8ccf49d915a3723052e338a1fef63d7e264ac1c5) 发布 [`v1.2.7`](https://github.com/HSOS6Miyabi/FFMPEG-UI/releases/tag/v1.2.7)。
- 发布文件包括 Windows x64 NSIS 安装程序、Windows x86_64 便携 ZIP 和 Linux x86_64 tar.gz。

### 2026-11-17：初始跨平台应用与分发

- 建立 C++20 共享转换核心、Windows/Linux 桌面窗口、FFmpeg/ffprobe 探测、编码与文件验证。
- 支持多视频、文件夹、ZIP/PPTX、资源调度、音频与分辨率选择、任务清单及 SSH 远程处理。
- 建立 CI 构建、真实视频集成测试、Windows 打包、安装卸载检查和 GitHub Release 发布流程。

## 长期需要保持的设计与维护要求

| 主题 | 必须保持的行为 | 检查位置 |
|---|---|---|
| 视频裁剪 | 一个视频只修改自己的范围；压缩包内部视频使用正确的相对路径 | `app_controller.*`、`archive.*`、`preview.*`、媒体自检 |
| 任务导入 | 扫描目录和压缩包时界面仍可操作，发现的视频逐步显示 | `scanLoop`、`inspectArchiveVideos`、Windows/Linux 界面 |
| 队列恢复 | 用户退出后可恢复未完成任务及逐视频裁剪范围 | `loadQueue`、`saveQueueLocked`、`tests/core_tests.cpp` |
| 输出文件安全 | 临时输出通过检查后才保存到最终路径，失败时保留源文件 | `conversion.cpp`、`archive.cpp` |
| ZIP/PPTX 安全 | 拒绝危险路径和文件链接，限制条目数与展开数据量 | `archive.cpp`、归档媒体测试 |
| SSH 安全 | 保护凭据，限制临时文件访问，处理结束后清理远程目录 | `ssh.cpp`、`conversion.cpp`、SSH 集成测试 |
| 界面一致性 | Windows/Linux 的功能与输入规则一致，兼容 DPI 缩放和长文件名 | 两个 `*_ui.cpp`、自动截图 |
| 发布版本 | 两个平台测试通过且 CMake 版本未发布时才创建新 Release | `.github/workflows/build.yml`、`CMakeLists.txt` |

## 每次维护后如何登记

在本文件中按日期新增一节，写明功能及用户可见变化、重要代码位置、需要兼容的数据格式、执行过的测试和对应的 CI 运行链接。若涉及发布，则另外记录版本标签、发布源提交以及交付文件名。

尚未安排的功能可参考[产品功能评估](product-review.md)；实际操作细节见[构建、测试与发布手册](ci-release.md)。
