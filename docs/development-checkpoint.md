# 开发状态记录

更新时间：2026-11-19。本文件说明主分支的功能、可验证的测试结果和交接事项。开始开发前，先阅读[接管与维护手册](handover.md)、[项目维护台账](maintenance-ledger.md)及[程序结构](architecture.md)。

## 主分支与发布版本

| 项目 | 状态 |
|---|---|
| 产品 | 视频压缩工作台 |
| 主要分支 | `main` |
| 平台 | Windows x64、Linux x86_64 |
| 语言与界面 | C++20；Windows Win32/GDI；Linux X11/Xft |
| 视频与归档处理 | FFmpeg、ffprobe、libarchive |
| CMake 声明版本 | 1.2.7 |
| 最近一次已确认成功的完整主分支 CI | [运行 44469229573](https://github.com/HSOS6Miyabi/FFMPEG-UI/actions/runs/44469229573)，提交 `76217c941cc0063a786ce6bf524d3d7711c1722f` |
| 当前已发布标签 | [v1.2.7](https://github.com/HSOS6Miyabi/FFMPEG-UI/releases/tag/v1.2.7)，源提交 `8ccf49d915a3723052e338a1fef63d7e264ac1c5` |

主分支的新功能在 v1.2.7 正式发布之后继续增加。要向用户提供包含逐视频裁剪与异步扫描的新版安装包，需要先更新 CMake 版本号并执行完整 CI。发布操作参见[构建、测试与发布手册](ci-release.md)。

## 现有功能

- 输入：单个或多个视频、文件夹、ZIP 和 PPTX；支持命令行传入、选择文件、拖放和任务清单导入。
- 文件列表：目录遍历和 ZIP/PPTX 内部视频检查在后台执行，队列逐步显示发现的视频，允许用户在扫描时继续使用界面。
- 视频编码：H.264、H.265、VP9、AV1；FFmpeg 提供 libvvenc 时可使用 H.266。支持 CPU 以及对应编码器可用时的 NVIDIA NVENC、Intel Quick Sync 和 AMD AMF。
- 输出：MP4、MKV、WebM 或原容器；六档分辨率；八档压缩级别；CRF/QP 质量控制；自动编码、复制或移除音频。
- 裁剪：每个独立视频、ZIP/PPTX 内部视频分别保存起点和终点。Windows/Linux 原生裁剪器提供预览画面、三个游标、拖动操作和精确时间输入。
- 后台预览：视频时长读取、预览帧生成、压缩包内部视频临时提取在后台运行；窗口持续处理交互事件。
- 队列：自动并行调度、进度显示、暂停、继续、取消、失败任务重试、输出大小统计和重复处理检查。
- 数据保存：编码选项保存在 `settings.ini`；未完成任务和各视频裁剪范围保存在版本 2 的 `queue.ffq`；可读取版本 1 清单。
- 远程处理：SSH 密钥、SSH Agent 和密码认证；通过 SSH/SCP 将视频交给远程 Linux 编码，并清理临时目录。
- 安装：Windows NSIS 安装程序及便携 ZIP 内含 FFmpeg/ffprobe；Linux 提供 tar.gz，运行时需要系统 FFmpeg/ffprobe。
- 用户界面：深浅色外观、紧凑窗口、DPI 缩放、专注队列视图、失败任务筛选、中文路径与长文件名处理。

## 主要模块

| 模块 | 责任 |
|---|---|
| `src/core/conversion.*` | 编码器探测、FFmpeg 参数、进度、输出文件检查 |
| `src/core/app_controller.*` | 异步扫描、任务管理、保存和恢复、并行调度 |
| `src/core/archive.*` | ZIP/PPTX 文件目录、解压、媒体转换和重新打包 |
| `src/core/preview.*` | 异步视频时长探测、临时提取和预览帧 |
| `src/core/process.*` | 进程启动与暂停、继续、取消 |
| `src/core/ssh.*` | SSH 凭据、连接和文件传输 |
| `src/platform/windows_ui.cpp` | Win32/GDI 界面与裁剪窗口 |
| `src/platform/linux_ui.cpp` | X11/Xft 界面与裁剪窗口 |
| `src/main.cpp`、`tests/core_tests.cpp` | 自检入口与核心单元测试 |
| `.github/workflows/build.yml` | 双平台 CI、截图、打包和正式发布 |

## 已确认的自动化测试

[运行 44469229573](https://github.com/HSOS6Miyabi/FFMPEG-UI/actions/runs/44469229573) 在 2026-11-19 对提交 `76217c941cc0063a786ce6bf524d3d7711c1722f` 执行了以下检查并全部成功：

- Windows、Linux CMake 配置和 C++ 编译。
- `ctest` 核心单元测试。
- 真实 FFmpeg 视频转换、裁剪、ZIP/PPTX 处理和任务队列测试。
- Linux 临时 SSH 服务端的连接、认证、转换和清理测试。
- Windows NSIS 打包、安装卸载、随包 FFmpeg 检查和实际媒体测试。
- Windows/Linux 真实桌面窗口截图与交互测试；Linux 同时覆盖 DPI 缩放与紧凑窗口。

自动化 CI 无法代表所有显卡和驱动的硬件编码测试；需要对应设备进行独立验证。后续修改完成后，以修改提交对应的 CI 结果为准。

## 构建命令

Linux：

~~~bash
cmake --preset linux-release
cmake --build --preset linux-release --parallel
ctest --preset linux-release
./build/linux-release/video-compression-workbench --self-test
~~~

Windows：

~~~powershell
cmake --preset windows-release -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build --preset windows-release --parallel
ctest --preset windows-release
.\build\windows-release\video-compression-workbench.exe --self-test
~~~

完整依赖、Windows vcpkg 准备和发布操作参见[构建、测试与发布手册](ci-release.md)。

## 需要持续遵守的注意事项

1. 修改任务、裁剪或导入逻辑时，保留各视频的独立设置和后台扫描。避免在 Win32 或 X11 事件处理函数中执行长时间文件遍历、压缩包展开或 FFmpeg 解码。
2. 修改文件输出行为时，始终先生成临时文件，验证成功后再保存到最终位置。源视频、ZIP 和 PPTX 必须受到保护。
3. ZIP/PPTX 必须拒绝危险路径和文件链接；修改内部视频文件名后，更新相关的文本和 PowerPoint 引用。
4. 修改编码参数时，保持 FFmpeg 参数安全传递、容器兼容性、不同编码器的质量选项以及媒体处理标记。
5. 修改队列数据结构时，保留版本 1 的读取支持、版本 2 的逐视频裁剪保存，以及重新启动后的恢复行为。
6. 修改 SSH 功能时，不得泄露凭据；检查上传、下载、暂停、取消和远程临时目录清理。
7. 修改界面时，在两种操作系统上检查输入区域、队列选中状态、实际点击位置、缩放、深浅色主题和长文件名。
8. 每次重要修改都同步更新相关技术文档、[项目决策记录](decision-log.md)和[维护台账](maintenance-ledger.md)。

## 后续工作

计划中尚未实现的功能和优先级记录在[产品功能评估](product-review.md)。项目仍使用固定的八档压缩级别；尚未提供用户自定义编码预设、目标文件大小模式和逐任务覆盖全部编码设置的功能。已有逐视频独立裁剪能力应继续保留。
