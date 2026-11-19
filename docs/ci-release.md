# 构建、测试与发布手册

本文用于新开发者在 Windows、Linux 和 GitHub Actions 中重现构建、检查功能、生成安装包和发布新版本。项目当前采用 `CMakeLists.txt` 里的三段版本号，自动构建文件为 `.github/workflows/build.yml`。

## 支持的环境

| 环境 | 工具 | 主要用途 |
|---|---|---|
| Linux 开发 | C++20 编译器、CMake 3.25+、Ninja、X11、Xft、Fontconfig、libarchive、FFmpeg、ffprobe | 编译应用、运行核心测试及实际媒体处理 |
| Linux CI | GitHub Actions `ubuntu-24.04`，另安装 OpenSSH 服务端、Xvfb、xdotool、ImageMagick、CJK 字体 | 自动验证媒体、SSH 和实际界面 |
| Windows 开发 | Visual Studio 2022 C++ 工具、Windows SDK、CMake 3.25+、Ninja、vcpkg、libarchive | 编译原生 Windows 应用和运行测试 |
| Windows CI | GitHub Actions `windows-latest`、vcpkg、FFmpeg、NSIS | 编译、转换测试、安装卸载测试、界面截图及打包 |

Linux 在运行时需要系统提供 FFmpeg 和 ffprobe。Windows 发布的安装程序和便携 ZIP 包含固定的 FFmpeg 与 ffprobe；从源码构建时也可使用系统安装的 FFmpeg。使用 SSH 远程编码时，本机还需要 OpenSSH 客户端。

## 本地构建与测试

Linux：

~~~bash
cmake --preset linux-release
cmake --build --preset linux-release --parallel
ctest --preset linux-release
./build/linux-release/video-compression-workbench --self-test
~~~

Windows：在已配置 vcpkg 的开发者 PowerShell 中运行。

~~~powershell
vcpkg install --triplet x64-windows-static
cmake --preset windows-release -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build --preset windows-release --parallel
ctest --preset windows-release
.\build\windows-release\video-compression-workbench.exe --self-test
~~~

可选的本地 C++ 格式和静态检查：

~~~bash
scripts/format-cpp.sh --check
scripts/lint-cpp.sh build/linux-release
~~~

`ctest` 主要检查核心逻辑与配置保存；`--self-test` 会调用真实的 FFmpeg，检查媒体转换、裁剪、压缩包和实际输出。程序的 `--demo` 模式用于生成并保留演示媒体。某些编码器必须具备相应 FFmpeg 构建或真实硬件后才能验证。

## 自动化测试步骤

`.github/workflows/build.yml` 在以下情况运行：

- 向 `main` 推送提交。
- 创建指向 `main` 的 Pull Request。
- 手动使用 GitHub Actions 的 `workflow_dispatch`。
- 推送匹配 `v*` 的标签。

Linux 作业依次安装依赖、启动临时 SSH 服务端、配置和编译、执行 `ctest` 和 `--self-test`、运行 Xvfb 界面测试、上传截图及 Linux tar.gz。

Windows 作业依次安装依赖、配置和编译、执行 `ctest`、打包 Windows 便携 ZIP 和 NSIS 安装程序、测试安装卸载及 `--self-test`、运行原生界面截图测试并上传文件。

界面测试重点覆盖编码设置、队列选择、逐视频裁剪、可视化时间轴、窗口缩放、DPI、深浅色主题和长文件名。Linux 测试通过 Xvfb 与 xdotool 操作窗口，Windows 测试脚本为 `scripts/test-windows-ui.ps1`。图片保存在 CI 的 `artifacts/ui/` 并通过 GitHub Actions 上传。

## 检查 CI 结果

1. 打开 [GitHub Actions](https://github.com/HSOS6Miyabi/FFMPEG-UI/actions)，选择当前 `main` 提交对应的运行记录。
2. 分别查看 **Linux build and media tests** 与 **Windows build and media tests**。两项均成功后，检查 **Publish GitHub Release** 的结果。
3. 查看具体步骤的日志和界面截图。不要只根据“编译成功”判断全部功能完成；实际媒体测试、安装包测试和截图测试也需要成功。
4. Linux 依赖安装使用 Ubuntu 24.04，设置 HTTP(S) 请求超时并支持重复尝试；遇到软件源下载较慢时，先确定失败发生在依赖下载还是编译及测试，再根据对应步骤处理。
5. 上次已确认成功的完整验证为 [2026-11-19 的运行 44469229573](https://github.com/HSOS6Miyabi/FFMPEG-UI/actions/runs/44469229573)，对应提交 `76217c941cc0063a786ce6bf524d3d7711c1722f`。此后有新提交时，应检查该提交对应的新运行记录。

GitHub Actions 的截图和安装包作为作业文件提供，可能受平台保存期限限制。正式交付的版本应从 GitHub Release 获取。

## 发布规则

`CMakeLists.txt` 中的 `project(VideoCompressionWorkbench VERSION X.Y.Z ...)` 决定版本标签 `vX.Y.Z`。

- 发布作业依赖 Linux 与 Windows 两项作业成功；在 `main` 或 `v*` 标签推送时执行。
- 如果 GitHub Release 中已有同版本标签，工作流会保留既有版本，不自动替换下载文件。
- 发布新功能前，先把 CMake 项目版本号更新为**尚未发布**的版本，提交到 `main`，完成完整 CI 后再检查 Release。
- 手动创建标签时，标签名称必须与 CMake 项目版本一致，且标签应对应准备发布的提交。
- 正式发布包括三个交付文件：`video-compression-workbench-X.Y.Z-windows-x64-setup.exe`、`video-compression-workbench-windows-x86_64.zip`、`video-compression-workbench-linux-x86_64.tar.gz`。
- 记录发布标签及其实际源提交，并在[维护台账](maintenance-ledger.md)中更新版本和测试结果。

**当前版本说明（2026-11-19）：** `v1.2.7` 的正式 Release 来自提交 `8ccf49d915a3723052e338a1fef63d7e264ac1c5`。当前 `main` 后续增加了逐视频独立裁剪及异步扫描等功能，项目的 CMake 版本仍为 1.2.7。因此需要提高项目版本号，才能把这些后续功能生成新的正式 Release。

## Windows 安装包

安装 NSIS 后在 PowerShell 中执行：

~~~powershell
./scripts/package-windows.ps1
./scripts/test-windows-installer.ps1
~~~

打包脚本下载固定版本的 Windows FFmpeg 并检查校验值，将依赖许可文件一起放入发布包。安装包及其测试细节见[Windows 安装与第三方组件](windows-distribution.md)。

## 修改功能后的对应检查

| 修改内容 | 必须重点运行的测试 |
|---|---|
| 编码器、编码参数、容器兼容性 | `ctest`、`--self-test`、不同容器和音频设置的实际输出检查 |
| 文件夹/ZIP/PPTX 导入 | 异步扫描、内部视频列表、中文路径、压缩包安全与媒体处理测试 |
| 裁剪与画面预览 | 单视频与 ZIP/PPTX 内视频独立范围、拖动游标、时长验证、两个系统的截图 |
| 队列恢复及设置保存 | 旧版 `.ffq` 导入、版本 2 清单、重新启动恢复、输入去重 |
| SSH 认证和远程处理 | 本机/远程转换、认证方式、文件传输、远程进程停止和临时文件清理 |
| Windows 安装与卸载 | NSIS、内置 FFmpeg、快捷方式、注册表和实际视频测试 |
| 界面布局 | Win32 和 X11 的普通及紧凑窗口、DPI 缩放、滚动和鼠标点击位置 |

代码合并或直接提交主分支后，必须检查最新提交对应的完整 CI 结果；只有所有相关检查通过，才能标记该次交付完成。
