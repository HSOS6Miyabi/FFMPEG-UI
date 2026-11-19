# 开发者接管指南

本文帮助接手项目的开发者了解代码结构、构建方法、设计要求和发布流程。首次接管请按[接管与维护手册](handover.md)的顺序操作；重要修改及验证结果见[维护台账](maintenance-ledger.md)，详细测试与发布操作见[构建、测试与发布手册](ci-release.md)。

## 当前项目状态

- 项目名称：视频压缩工作台。
- CMake 项目版本：1.2.7。当前 `main` 已包含正式发布 v1.2.7 之后增加的功能；最新 Release 的源提交与当前 `main` 不同，发布新版需要更新 CMake 版本。
- 主要分支：`main`。
- 目标平台：Windows x64、Linux x86_64。
- 主要语言：C++20。
- 界面：Windows 原生 Win32/GDI；Linux X11/Xft。
- 视频处理：通过 FFmpeg 和 ffprobe 命令行程序完成。
- 远程处理：通过本机 OpenSSH 客户端调用远程 Linux 服务器上的 FFmpeg。
- 输出容器：支持 MP4、MKV、WebM 和沿用源文件的容器格式。
- ZIP/PPTX：通过 libarchive 读取和生成。
- 项目不使用 Qt、GTK、MFC 或 Electron 等桌面界面框架。

主分支包含独立视频、文件夹、ZIP 和 PPTX 批量处理及后台扫描；MP4、MKV、WebM 和原格式四种输出模式；H.264、H.265、VP9、AV1 及条件支持的 H.266；CPU、NVIDIA NVENC、Intel Quick Sync 和 AMD AMF 编码器探测；八档压缩级别；六档输出分辨率；自动编码、保留原音频和移除音频三种音频处理方式；逐视频独立裁剪和跨平台可拖动时间轴；后台预览；SSH 远程编码；任务进度、暂停、继续和取消；自适应并行调度；重复处理检查；任务清单恢复与导入导出；Windows 和 Linux 文件拖放。

## 目录说明

| 路径 | 用途 |
|---|---|
| `src/core/conversion.*` | FFmpeg 参数生成、编码器探测、视频转换、进度读取和输出验证 |
| `src/core/process.*` | Windows/Linux 子进程启动、输出读取、环境变量和暂停、继续、取消 |
| `src/core/ssh.*` | SSH 配置读取与同步、认证凭据、远程命令和文件传输 |
| `src/core/archive.*` | ZIP/PPTX 解包、视频替换、引用更新、重新打包和完整性检查 |
| `src/core/app_controller.*` | 后台目录和压缩包扫描、任务队列自动恢复、逐视频裁剪保存、并行调度和状态 |
| `src/core/preview.*` | 后台读取视频时长、提取 ZIP/PPTX 内部视频和预览画面 |
| `src/platform/windows_ui.cpp` | Windows Win32/GDI 原生界面及视频裁剪窗口 |
| `src/platform/linux_ui.cpp` | Linux X11/Xft 原生界面及视频裁剪窗口 |
| `src/main.cpp` | 应用入口、命令行参数、自检和演示模式 |
| `tests/core_tests.cpp` | 核心逻辑单元测试 |
| `.github/workflows/build.yml` | Windows/Linux CI、桌面截图、NSIS 安装包测试及经测试的版本发布 |
| `packaging/windows-installer.nsi` | NSIS 安装、卸载、快捷方式和软件注册表信息 |
| `scripts/package-windows.ps1` | 固定版本 FFmpeg 下载校验、静态依赖许可文件整理和 Windows 发布包生成 |
| `scripts/generate-windows-installer-art.ps1` | 根据项目图标生成 NSIS 安装向导的欢迎页和页眉插图 |
| `scripts/test-windows-installer.ps1` | Windows EXE 版本资源、静默安装、内置 FFmpeg、媒体测试和卸载验证 |
| `src/platform/windows_version.rc.in` | Windows 应用版本信息和 Kenvix 作者版权字段 |
| `docs/architecture.md` | 组件结构和处理流程 |
| `docs/queue-format.md` | `.ffq` 版本 1/2 格式、裁剪范围保存与恢复 |
| `docs/handover.md` | 新开发者接管顺序和最小验证步骤 |
| `docs/maintenance-ledger.md` | 已完成的重要工作和验证记录 |
| `docs/ci-release.md` | Windows/Linux 构建、CI、打包及发布步骤 |
| `docs/decision-log.md` | 重要设计决策和维护约束 |
| `docs/encoding.md` | 编码质量、预设、音频处理、分辨率和进度参数 |
| `docs/ssh-remote-processing.md` | SSH 远程处理、认证、安全和测试要求 |
| `.clang-format`、`.clang-tidy`、`scripts/` | C++ 格式规范、静态检查规则和本地检查脚本 |

## 构建环境

### Linux

需要 C++20 编译器、CMake 3.25 或更新版本、Ninja、X11、Xft、Fontconfig、libarchive、FFmpeg 和 ffprobe。远程编码还需要 OpenSSH 客户端。

Ubuntu/Debian 可安装依赖：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build libx11-dev libxft-dev libfontconfig1 libarchive-dev ffmpeg
```

构建和测试：

```bash
cmake --preset linux-release
cmake --build --preset linux-release --parallel
ctest --preset linux-release
./build/linux-release/video-compression-workbench --self-test
```

### Windows

需要 Visual Studio 2022 C++ 桌面开发工具、Windows SDK、CMake 3.25 或更新版本、vcpkg、libarchive 和 FFmpeg/ffprobe。远程编码还需要 Windows OpenSSH Client。

在 PowerShell 中执行：

```powershell
cmake --preset windows-release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build --preset windows-release --parallel
ctest --preset windows-release
.\build\windows-release\video-compression-workbench.exe --self-test
```

vcpkg 依赖由 `vcpkg.json` 声明。Windows NSIS 安装程序和便携 ZIP 已包含固定版本的 FFmpeg 和 ffprobe，源码直接运行时可以使用系统 FFmpeg。打包规范、第三方许可证及测试方式记录在 [Windows 安装与第三方组件](windows-distribution.md)。

## 裁剪、后台扫描与任务保存

- 视频裁剪范围保存于各自的 `BatchItemSnapshot::trim`，通过 `AppController::setVideoTrim` 修改。独立视频传空的内部文件名，ZIP/PPTX 内视频传对应的相对路径。任务队列恢复、导入和导出须保留每个视频的裁剪范围。保存格式见[任务清单格式](queue-format.md)。
- `AppController::scanLoop` 负责文件夹扫描和压缩包内部视频列表读取；`initializeNow` 在后台检测 FFmpeg 编码器；`AsyncVideoPreview` 在后台读取时长、提取单个内部视频并生成预览。Windows 和 Linux 界面要在等待时显示进展并继续响应窗口事件，扫描未完成前不能启动批处理。
- 可视化裁剪器显示三种游标：起点、终点和画面预览位置。调整起点和终点时须保证结束时间晚于起始时间。选中另一个视频后，应独立展示其裁剪范围。
- FFmpeg 的 `-ss` 必须出现在 `-i` 之前，指定结束时间时，输出侧使用 `-t` 表示持续时间。ZIP/PPTX 应针对内部各视频使用各自的参数，重复处理检查须包含这些参数。
- 运行 `ctest` 和 `--self-test`，检查单视频裁剪、ZIP/PPTX 内视频独立裁剪、任务队列恢复、后台扫描、两个系统的界面截图，以及 Windows 安装包卸载后的快捷方式处理。最近一次已确认成功的完整验证见[CI 运行 44469229573](https://github.com/HSOS6Miyabi/FFMPEG-UI/actions/runs/44469229573)。


## 运行与调试

```bash
video-compression-workbench --encoders
video-compression-workbench --self-test
video-compression-workbench --demo
video-compression-workbench /path/to/video.mp4 /path/to/input.zip /path/to/slides.pptx
```

Windows 文件关联或“发送到”菜单可以直接把一个或多个路径作为命令行参数传给程序。开发时可设置 `FFMPEG_PATH` 和 `FFPROBE_PATH` 指向指定的可执行文件。

## 重要实现约束

1. **不要通过 shell 拼接 FFmpeg 命令。** 所有路径和选项必须作为独立参数传给 `ProcessRunner`，避免空格、Unicode 路径和命令注入问题。
2. **修改编码参数或容器规则时同步更新测试和说明。** 检查 `src/core/conversion.cpp`、`src/core/conversion.hpp`、`tests/core_tests.cpp` 和 `docs/encoding.md`。硬件编码器的参数不能直接套用 CPU 编码器的 CRF 参数；输出容器必须与视频和音频编码兼容。
3. **编码器必须按格式和设备分别探测。** 某个设备支持 H.264，不代表它也支持 H.265 或 AV1。界面只显示当前 FFmpeg 构建和驱动实际探测成功的选项。
4. **不要直接覆盖输入文件。** 视频和归档文件必须先写入临时文件，完成格式及完整性检查后才保存到最终路径。
5. **修改 ZIP/PPTX 时同步更新媒体引用。** 视频扩展名变更后，检查文本资源引用、PPTX `.rels` 的 `Target` 和 `[Content_Types].xml`。任何内部视频失败都应放弃当前临时归档，并保留原始文件。
6. **保留归档安全限制。** 不允许绝对路径、路径穿越、符号链接和硬链接；条目数、单文件大小和总展开大小限制应持续生效。
7. **避免并行任务嵌套过量。** 资源调度器负责顶层任务和容器内部视频的并发分配。修改调度策略时，必须检查 CPU 线程、内存估算、硬件编码并发限制和取消行为。
8. **Windows 与 Linux 界面保持功能一致。** 新增控件、参数和任务状态时，检查两套平台实现。Windows 需保留 Per-Monitor V2 DPI、Common Controls v6、Segoe UI 和 DWM 外观；Linux 需保留 Xft 字体缩放和 Xdnd 文件拖放。
9. **用户设置必须兼容默认值。** 新增配置字段时，旧配置文件缺少该字段也必须能够正常启动。
10. **重要设计修改必须写入决策记录。** 更新 `docs/decision-log.md`，并按需同步架构、编码说明和 README。

## 测试流程

提交前执行：

```bash
cmake --build --preset linux-release --parallel
ctest --preset linux-release
./build/linux-release/video-compression-workbench --self-test
```

`ctest` 运行核心单元测试，覆盖 SSH 配置解析、凭据保存与删除、编码参数映射、音频处理方式、分辨率滤镜、任务列表恢复、归档路径安全和任务调度。`--self-test` 使用本机 FFmpeg 创建测试媒体，检查 CPU 编码器、音频复制与移除、分辨率缩小、进度解析、重复处理标记、ZIP/PPTX 替换、并行处理和远程处理。Linux CI 还会启动临时 OpenSSH 服务器，验证密钥认证、密码认证、SCP 文件传输、远程编码和临时目录清理。缺少的可选编码器会跳过。硬件编码器需要真实设备和驱动；没有设备时，验证对应选项不会出现在界面中。

Windows 与 Linux 的完整构建和测试由 GitHub Actions 执行。开发机器缺少某类硬件时，不要把硬件编码器测试描述为已通过。

## 本地代码质量检查

C++ 格式规则保存在 `.clang-format`。 本地配置文件 `.clang-format` 和 `.clang-tidy` 分别规定代码格式与静态检查项目。CMake 的 Linux 和 Windows 构建预设都会生成 `compile_commands.json`，可直接供 clang-tidy 使用。在开发机安装 LLVM `clang-format` 和 `clang-tidy` 后，使用以下命令进行检查：

```bash
scripts/format-cpp.sh --check
cmake --preset linux-release
scripts/lint-cpp.sh build/linux-release
```

需要格式化代码时执行 `scripts/format-cpp.sh --write`。这些检查由开发者在本地执行，不放入 GitHub Actions。

## 格式兼容与竖屏处理

- `VideoCodec` 枚举保留旧数值：H.264=0、H.265=1、AV1=2。VP9=3、H.266=4。界面按 H.264、H.265、VP9、AV1、H.266 排列。
- `OutputContainer` 枚举保留 MP4=0 和 SameAsSource=1，新增 MKV=2 和 WebM=3。旧设置文件可继续读取。
- H.266 仅在 FFmpeg 提供 `libvvenc` 时启用，使用 QP 与 10-bit 像素格式。VP9 使用 `libvpx-vp9`。容器兼容性检查和音频选择由共享核心处理。
- 分辨率设置根据输入画面宽高决定横竖屏限制，保持宽高比，不放大画面。测试需包含 1080×1920 输入在 1080p 和 480p 档位下的实际输出。
- Windows 和 Linux 的控件宽度随 DPI 计算，队列状态与完整进度信息分行显示；调整队列布局时同步修改鼠标选中行与双击位置的计算。

## CI 与发布

- Linux CI 运行在 Ubuntu 24.04，依赖安装包含超时与重试；Windows CI 运行在 `windows-latest`。具体工具、日志检查方法和打包步骤见[构建、测试与发布手册](ci-release.md)。
- 推送到 `main` 或创建针对 `main` 的 Pull Request 时，工作流会构建 Windows 和 Linux 版本，并执行单元测试、媒体自检和真实桌面界面测试。
- `workflow_dispatch` 可手动运行工作流。
- `main` 分支的 Linux 和 Windows 测试全部成功后，发布任务读取 `CMakeLists.txt` 中的版本号，从通过验证的提交创建版本标签和 GitHub Release，并上传 Windows NSIS 安装程序、Windows 便携 ZIP 和 Linux tar.gz。
- 已发布的版本不会被自动覆盖。当前已发布的 v1.2.7 源提交为 `8ccf49d915a3723052e338a1fef63d7e264ac1c5`，后续主分支功能尚需通过提高 CMake 版本号才能正式发布。
- 手动推送与 CMake 项目版本一致的 `v*` 标签时，也会执行完整验证；发布前必须确保两个平台的检查通过。

## 界面检查

编码设置面板在 Windows 和 Linux 上采用双列布局：输出容器与视频编码并排，编码设备与目标质量并排；展开高级设置后，压缩级别滑块与音频处理并排，音频码率与输出分辨率并排。 在窗口右上方可以切换队列专注视图，压缩设置区在专注视图中不显示，批量控制按钮和右侧任务详情继续可用。队列可只显示失败任务，所有用户操作都必须使用当前筛选后的任务顺序计算所选项目。压缩级别滑块有八个固定档位，须保留每个档位的原有参数含义和设置文件数值。调整界面后，应在 Linux 和 Windows 上重新启动应用并检查实际窗口截图，重点检查输出目录输入框、编码设备选项、CRF、压缩级别、音频码率、输出分辨率、音频处理方式和底部任务队列是否发生重叠。还应检查常用分辨率、窗口缩放、深浅色主题和长文件名显示。Windows 界面需要检查 Per-Monitor V2 DPI、系统主题和 DWM 效果。
