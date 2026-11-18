# Windows 安装与第三方组件

## 安装程序

Windows 发布时同时生成以下文件：

- `video-compression-workbench-1.2.7-windows-x64-setup.exe`：NSIS 安装程序。
- `video-compression-workbench-windows-x86_64.zip`：可直接解压运行的版本。

安装程序默认安装到当前用户的 `%LOCALAPPDATA%\Programs\VideoCompressionWorkbench`，无需管理员权限。安装时会创建开始菜单快捷方式，并可选择创建桌面快捷方式。可以通过 Windows“已安装的应用”或者开始菜单卸载程序。

NSIS 安装和卸载程序均使用产品图标。安装向导采用与主程序一致的靛蓝色欢迎页插图和浅色页面横幅。安装时会自动使用包含多种尺寸的图标资源。

安装程序和应用可执行文件的版本信息包括产品名称“视频压缩工作台”、发行者 Kenvix、作者联系邮箱 `i@kenvix.com`，以及对应的产品版本号。

### 应用界面和硬件编码器检测

主程序以 Windows 图形应用方式启动，双击打开时不会显示命令提示符窗口。通过命令行参数 `--encoders` 和 `--self-test` 运行时，可将输出重定向到文件。

主窗口采用双列布局，控制项之间保留独立的标签间距和行间距：输出容器与视频编码位于同一行，编码设备与目标质量位于同一行。点击“展开高级设置”后，可使用八档离散滑块选择压缩级别，并在相邻一列设置音频处理方式；音频码率和输出分辨率也并排显示。压缩级别可以通过点击、拖动滑块和方向键调整，既有编码参数与存储格式保持不变。处理队列支持单击选择、Delete 键移除、清空列表以及鼠标滚轮浏览。 可以从窗口右上角切换为队列专注视图，筛选失败任务，并在右侧查看选中任务的来源文件、输出位置和状态。Ctrl+Q 可以切换视图，Ctrl+O 添加文件，Ctrl+D 清除已完成任务。失败任务会显示具体原因，在 Windows 上双击任务可打开完整详情。清空队列时不会删除源文件和已有的输出文件。

软件会分别测试 H.264、H.265 和 AV1 的 NVIDIA NVENC 编码器。点击“检测详情”可查看 FFmpeg 返回的具体错误信息；更新显卡驱动或调整 Windows 图形首选项后，可点击“重新检测”。如果当前 GPU 或驱动不支持某种编码格式，相应的编码器不会作为可用选项显示。

### 视频格式与屏幕适配

Windows 界面提供 MP4、MKV、WebM 和原格式四种输出方式，编码列表按年代显示 H.264、H.265、VP9、AV1 与 H.266。VP9 使用 `libvpx-vp9`；H.266 需要 `libvvenc`，当前 FFmpeg 缺少该编码器时会在界面中禁用相关选项。竖屏视频选择输出分辨率时会使用对应的竖屏限制，保持原有画面方向。处理队列的状态与进度说明分行显示，避免在高 DPI 显示器上相互遮挡。

## 随包提供的组件

安装目录中的 `bin` 文件夹包括：

- `ffmpeg.exe`：执行视频和音频转换。
- `ffprobe.exe`：探测视频参数和验证转换结果。

程序优先使用安装目录中的这两个文件。通过 `FFMPEG_PATH` 和 `FFPROBE_PATH` 环境变量仍可指定其他可执行文件。解压版使用同一套查找规则。

Windows 使用 libarchive 的宽字符接口访问 ZIP 和 PPTX 压缩包，支持中文源文件名、中文输出路径及压缩包内部的 Unicode 文件名；媒体引用更新时使用 UTF-8 编码保存路径。处理 PPTX 时保留原文件，将输出写入新文件。

应用使用静态链接的 C++ 运行库和 libarchive，因此无需另外安装这两项组件。SSH 远程编码使用 Windows 的 OpenSSH Client；只有使用远程处理功能时才需要在 Windows 可选功能中启用该组件。

## FFmpeg 来源和许可证

Windows 发布包中的 FFmpeg 固定为 **9.0.2 Essentials x64 静态构建**，由 Gyan Doshi 提供。该构建采用 **GNU GPL v3** 许可证，包含独立开源组件及相应许可条款。

- 构建页面：https://www.gyan.dev/ffmpeg/builds/
- 固定版本：https://github.com/GyanD/codexffmpeg/releases/tag/9.0.2
- 构建文件：`ffmpeg-9.0.2-essentials_build.zip`
- SHA-256：`60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba`
- FFmpeg 源代码：https://ffmpeg.org/download.html
- FFmpeg 法律说明：https://ffmpeg.org/legal.html

打包脚本会验证下载文件的 SHA-256。校验未通过时，构建将停止。FFmpeg 原始发行包的许可证文件会复制到 `docs/third-party/FFmpeg`，通过 vcpkg 静态链接的组件许可证会复制到 `docs/third-party/vcpkg`。

应用自身的作者和版权信息归 Kenvix 所有。FFmpeg 和其他第三方组件保留各自的作者、版权及许可证。

如需再分发安装程序，应确保对应 FFmpeg 构建、所使用第三方库及其构建方式所要求的源代码能够按照 GPL 的规定获取。对于固定版本和依赖项的更改，需要同时更新打包脚本、许可证文件和本说明。

## 构建和测试

先在 Windows 上使用 vcpkg 配置并构建应用，再执行：

```powershell
choco install nsis -y --no-progress
./scripts/package-windows.ps1
./scripts/test-windows-installer.ps1
```

打包脚本下载固定版本的 FFmpeg，检查文件校验值，生成带 FFmpeg 的便携 ZIP 和 NSIS 安装程序。测试脚本检查 EXE 和安装程序版本资源、静默安装、注册表信息、内置 FFmpeg、视频转换自检及静默卸载。

GitHub Actions 会自动执行上述打包和测试步骤。两个平台的构建与测试均通过后，才会发布 Windows 安装程序、Windows ZIP 及 Linux tar.gz。
