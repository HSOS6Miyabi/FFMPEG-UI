# Windows 安装与第三方组件

## 安装程序

Windows 发布时同时生成以下文件：

- `video-compression-workbench-1.2.1-windows-x64-setup.exe`：NSIS 安装程序。
- `video-compression-workbench-windows-x86_64.zip`：可直接解压运行的版本。

安装程序默认安装到当前用户的 `%LOCALAPPDATA%\Programs\VideoCompressionWorkbench`，无需管理员权限。安装时会创建开始菜单快捷方式，并可选择创建桌面快捷方式。可以通过 Windows“已安装的应用”或者开始菜单卸载程序。

NSIS 安装和卸载程序均使用产品图标。安装向导采用与主程序一致的靛蓝色欢迎页插图和浅色页面横幅。安装时会自动使用包含多种尺寸的图标资源。

安装程序和应用可执行文件的版本信息包括产品名称“视频压缩工作台”、发行者 Kenvix、作者联系邮箱 `i@kenvix.com`，以及对应的产品版本号。

### 应用界面和硬件编码器检测

主程序以 Windows 图形应用方式启动，双击打开时不会显示命令提示符窗口。通过命令行参数 `--encoders` 和 `--self-test` 运行时，可将输出重定向到文件。

主窗口默认展示常用的输出文件夹、容器、视频编码、编码设备和质量设置。点击“展开高级设置”可调整压缩级别、音频参数、分辨率以及批处理选项。处理队列支持单击选择、Delete 键移除、清空列表以及鼠标滚轮浏览。清空队列时不会删除源文件和已有的输出文件。

软件会分别测试 H.264、H.265 和 AV1 的 NVIDIA NVENC 编码器。点击“检测详情”可查看 FFmpeg 返回的具体错误信息；更新显卡驱动或调整 Windows 图形首选项后，可点击“重新检测”。如果当前 GPU 或驱动不支持某种编码格式，相应的编码器不会作为可用选项显示。

## 随包提供的组件

安装目录中的 `bin` 文件夹包括：

- `ffmpeg.exe`：执行视频和音频转换。
- `ffprobe.exe`：探测视频参数和验证转换结果。

程序优先使用安装目录中的这两个文件。通过 `FFMPEG_PATH` 和 `FFPROBE_PATH` 环境变量仍可指定其他可执行文件。解压版使用同一套查找规则。

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
