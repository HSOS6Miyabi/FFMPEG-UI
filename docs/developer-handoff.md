# 开发者接管指南

本文用于帮助接手项目的开发者快速了解代码结构、构建方法、重要设计约束和发布流程。开始修改前，请先阅读本文、[程序结构](architecture.md)、[项目决策记录](decision-log.md)、[编码参数说明](encoding.md)和[产品功能评估](product-review.md)。

## 当前项目状态

- 项目名称：视频压缩工作台。
- 当前开发版本：1.2.2。
- 主要分支：`main`。
- 目标平台：Windows x64、Linux x86_64。
- 主要语言：C++20。
- 界面：Windows 原生 Win32/GDI；Linux X11/Xft。
- 视频处理：通过 FFmpeg 和 ffprobe 命令行程序完成。
- 远程处理：通过本机 OpenSSH 客户端调用远程 Linux 服务器上的 FFmpeg。
- 输出容器：支持统一输出 MP4，也支持每个视频沿用源文件的容器格式。
- ZIP/PPTX：通过 libarchive 读取和生成。
- 项目不使用 Qt、GTK、MFC 或 Electron 等桌面界面框架。

当前版本包含独立视频、文件夹、ZIP 和 PPTX 批量处理；MP4 和源文件容器两种输出模式；H.264、H.265、AV1；CPU、NVIDIA NVENC、Intel Quick Sync 和 AMD AMF 编码器探测；八档压缩级别；六档输出分辨率；自动编码、保留原音频和移除音频三种音频处理方式；SSH 远程编码；任务进度、暂停、继续和取消；自适应并行调度；重复处理检查；任务列表自动恢复和手动导入导出；Windows 命令行文件参数和文件拖放。

## 目录说明

| 路径 | 用途 |
|---|---|
| `src/core/conversion.*` | FFmpeg 参数生成、编码器探测、视频转换、进度读取和输出验证 |
| `src/core/process.*` | Windows/Linux 子进程启动、输出读取、环境变量和暂停、继续、取消 |
| `src/core/ssh.*` | SSH 配置读取与同步、认证凭据、远程命令和文件传输 |
| `src/core/archive.*` | ZIP/PPTX 解包、视频替换、引用更新、重新打包和完整性检查 |
| `src/core/app_controller.*` | 输入扫描、任务队列自动恢复、并行调度、设置保存和任务状态 |
| `src/platform/windows_ui.cpp` | Windows 原生界面 |
| `src/platform/linux_ui.cpp` | Linux X11/Xft 界面 |
| `src/main.cpp` | 应用入口、命令行参数、自检和演示模式 |
| `tests/core_tests.cpp` | 核心逻辑单元测试 |
| `.github/workflows/build.yml` | Windows/Linux CI、桌面截图、NSIS 安装包测试及经测试的版本发布 |
| `packaging/windows-installer.nsi` | NSIS 安装、卸载、快捷方式和软件注册表信息 |
| `scripts/package-windows.ps1` | 固定版本 FFmpeg 下载校验、静态依赖许可文件整理和 Windows 发布包生成 |
| `scripts/generate-windows-installer-art.ps1` | 根据项目图标生成 NSIS 安装向导的欢迎页和页眉插图 |
| `scripts/test-windows-installer.ps1` | Windows EXE 版本资源、静默安装、内置 FFmpeg、媒体测试和卸载验证 |
| `src/platform/windows_version.rc.in` | Windows 应用版本信息和 Kenvix 作者版权字段 |
| `docs/architecture.md` | 组件结构和处理流程 |
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

## CI 与发布

- 推送到 `main` 或创建针对 `main` 的 Pull Request 时，工作流会构建 Windows 和 Linux 版本，并执行单元测试及媒体自检。
- `workflow_dispatch` 可手动运行工作流。
- `main` 分支的 Linux 和 Windows 测试全部成功后，发布任务读取 `CMakeLists.txt` 中的版本号，从通过验证的提交创建版本标签和 GitHub Release，并上传 Windows NSIS 安装程序、Windows 便携 ZIP 和 Linux tar.gz。
- 已发布的版本不会被自动覆盖。需要发布新版本时，先更新 CMake 项目的版本号，再提交到 `main`。
- 手动推送与 CMake 项目版本一致的 `v*` 标签时，也会执行完整验证；发布前必须确保两个平台的检查通过。

## 界面检查

编码设置面板在 Windows 和 Linux 上采用双列布局：输出容器与视频编码并排，编码设备与目标质量并排；展开高级设置后，压缩级别滑块与音频处理并排，音频码率与输出分辨率并排。压缩级别滑块有八个固定档位，须保留每个档位的原有参数含义和设置文件数值。调整界面后，应在 Linux 和 Windows 上重新启动应用并检查实际窗口截图，重点检查输出目录输入框、编码设备选项、CRF、压缩级别、音频码率、输出分辨率、音频处理方式和底部任务队列是否发生重叠。还应检查常用分辨率、窗口缩放、深浅色主题和长文件名显示。Windows 界面需要检查 Per-Monitor V2 DPI、系统主题和 DWM 效果。
