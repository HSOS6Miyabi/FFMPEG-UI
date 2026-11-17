# 开发状态记录

本文记录当前主线的实现情况、构建方式和后续维护要求。开始修改前，还应阅读程序结构、项目决策记录、编码参数说明、SSH 远程处理说明和产品功能评估。

## 当前产品

- 产品名称：视频压缩工作台
- 支持平台：Windows x64、Linux x86_64
- 主要实现语言：C++20
- 视频处理依赖：FFmpeg、ffprobe
- 压缩包处理依赖：libarchive
- 主线分支：main
- CMake 可执行文件输出名：video-compression-workbench

## 已实现功能

- 单个视频、多个视频、文件夹、ZIP 和 PPTX 批量处理。
- H.264、H.265 和 AV1 视频编码。
- CPU、NVIDIA NVENC、Intel Quick Sync 和 AMD AMF 编码器探测。
- CRF 数值、八档压缩级别和 MP4 / 源文件容器输出。
- 原始尺寸、2160p、1440p、1080p、720p 和 480p 六档输出分辨率。
- 自动编码、保留原音频和移除音频三种音频处理方式。
- 批量任务队列、并行调度、暂停、继续、取消和重复处理检查。
- 自动保存未完成任务，重新启动后恢复任务列表。
- 手动导入和导出 .ffq 任务清单。
- SSH 密钥、SSH Agent 和密码认证。
- SSH 远程 Linux 编码、SCP 上传下载、进度读取和临时目录清理。
- Windows 和 Linux 原生桌面界面，支持深浅色外观。
- 任务进度、输出文件大小和压缩结果统计。

## 构建方式

Linux：

```bash
cmake --preset linux-release
cmake --build --preset linux-release --parallel
ctest --preset linux-release
./build/linux-release/video-compression-workbench --self-test
```

Windows：

```powershell
cmake --preset windows-release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build --preset windows-release --parallel
ctest --preset windows-release
.\build\windows-release\video-compression-workbench.exe --self-test
```

系统需要安装 FFmpeg 和 ffprobe，并确保程序可以从 PATH 中找到它们。Windows 构建还需要通过 vcpkg 安装 libarchive。

## 自动化测试

GitHub Actions 在 Linux 和 Windows 上分别执行 CMake 配置、编译、核心单元测试、FFmpeg 媒体测试和发布包生成。

Linux CI 会启动临时 OpenSSH 服务器，验证密钥认证、密码认证、SCP 文件传输、远程 FFmpeg 能力探测、远程编码、进度读取和临时目录清理。

核心测试覆盖 SSH 配置读写、凭据保存、音频处理方式、输出分辨率、任务列表恢复、参数指纹、归档路径安全和任务调度。

硬件编码需要真实 NVIDIA、Intel 或 AMD 设备和对应驱动。CI 的 CPU 编码测试不能替代硬件设备验证。

## 界面检查

编码设置区域使用左侧参数名称、右侧控件的布局。检查界面时，重点确认以下控件没有发生重叠：

- 输出文件夹和输出容器
- 视频编码格式和编码设备
- CRF 数值与滑块
- 两行压缩级别
- 音频码率
- 输出分辨率
- 音频处理方式
- 并行任务和文件处理选项
- 底部任务列表

Linux CI 会使用 Xvfb 启动桌面程序，选择 1080p 和保留原音频选项，并保存实际窗口截图供检查。Windows 界面还需要在真实桌面环境中检查 DPI 缩放、系统主题和窗口尺寸。

## 已知范围

- 当前程序要求用户自行安装 FFmpeg 和 ffprobe，发布包不包含这两个依赖。
- 字幕流和数据流默认不复制。
- 当前提供固定的八档压缩级别，尚未提供用户自定义编码预设。
- 当前没有内置视频播放器、目标文件大小模式和逐任务独立编码设置。
- 产品功能评估文档记录了后续建议和优先级。

## 持续维护要求

- 修改前重新读取主线中的文件，保留用户已经做出的修改。
- 共享编码参数必须放在 core 层，Windows 和 Linux 界面只负责显示和收集用户选择。
- 新增参数必须参与设置保存、输出文件命名、处理参数指纹、直接视频测试和 ZIP/PPTX 测试。
- 远程处理必须引用并安全转义所有用户输入，密码不得写入命令行参数、日志或 SSH 配置文件。
- 每个远程任务必须使用独立临时目录，并在成功、失败和取消时清理。
- 输出文件先写入临时文件，验证容器、视频编码、音频处理和分辨率后再替换最终文件。
- 修改界面后重新构建并检查 Windows 和 Linux 的实际窗口截图。
- 架构、持久化行为或用户可见参数发生变化时，同步更新 README、编码说明、决策记录和产品功能评估。
