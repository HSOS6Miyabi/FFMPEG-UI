<p align="center">
  <img src="assets/icons/video-compression-workbench.png" alt="视频压缩工作台应用图标" width="128" height="128">
</p>

# 视频压缩工作台

视频压缩工作台是一款面向 Windows 和 Linux 的桌面视频压缩工具，支持单文件、批量文件、文件夹、ZIP 压缩包和 PowerPoint 演示文稿。可以选择 MP4、MKV、WebM 输出，也可以沿用源文件的容器格式；支持 H.264、H.265、VP9 和 AV1，启用 libvvenc 的 FFmpeg 还可使用 H.266，并提供输出分辨率、音频处理方式、CRF 数值和压缩级别等设置。

程序使用 C++20 编写。Windows 界面使用 Win32 API 和 GDI，Linux 界面使用 X11 和 Xft。程序通过 FFmpeg 和 ffprobe 完成视频编码，通过 libarchive 读取和生成 ZIP 文件。项目不使用 Qt、GTK、MFC、Electron 等桌面界面框架。默认在本机处理视频；选择 SSH 服务器后，程序只会临时传输当前任务所需的视频，完成后自动将结果下载到本机。

图标资源：[SVG 矢量图](assets/icons/video-compression-workbench.svg) · [PNG 高清图](assets/icons/video-compression-workbench.png) · [Windows ICO 图标](assets/icons/video-compression-workbench.ico)。

## 功能概览

- **多种输入方式：** 添加单个或多个视频、ZIP、PPTX，也可以添加整个文件夹。支持将文件直接拖入窗口。
- **批量任务队列：** 每个视频单独显示状态和进度；添加 ZIP、PPTX 后会立即在后台读取内部文件目录，并逐步列出其中的视频。可以切换为专注队列视图，扩大列表显示区域，也可以只显示失败的任务。选中任务后，右侧显示来源文件、输出位置、任务状态和错误信息；Windows 可通过双击任务查看完整详情。
- **文件夹扫描：** 默认递归扫描子文件夹，可关闭递归扫描。扫描在后台进行，发现的文件会逐步显示在队列中，界面可以继续操作。指定输出文件夹后，可以保留原有目录结构。
- **ZIP 处理：** 展开 ZIP，扫描其中的视频，按照所选输出容器转换后替换原视频并重新生成 ZIP。其他文件和目录会保留，常见文本、XML 和网页文件中的视频路径也会随文件名变化而更新。
- **Unicode 文件路径：** Windows 使用宽字符文件路径读取和创建 ZIP/PPTX，支持中文压缩包文件名、内部媒体名称及中文输出文件夹。
- **PPTX 处理：** 读取 PPTX 内部 ZIP 包，逐个转换嵌入视频，更新幻灯片关系文件和对应的媒体内容类型，再生成新的 PPTX。原始演示文稿始终保留。
- **三家硬件编码器：** 自动检测 NVIDIA NVENC、Intel Quick Sync 和 AMD AMF。程序会检查 FFmpeg 编码器列表，并运行短片段编码测试；当前设备无法实际使用的编码器不会显示为可选项。
- **可视化视频裁剪：** 在队列中选中视频后，主界面可以直接填写该视频的起始和结束时间；裁剪器支持画面预览、拖动起点与终点、拖动预览游标、前进与后退、精确输入时间。每个视频独立保存裁剪时间，包括 ZIP 和 PPTX 内的视频。
- **八档压缩级别：** 从极速到极致压缩，Windows 和 Linux 界面使用带刻度的离散滑块，可以点击刻度、拖动滑块或使用方向键选择档位。CRF 数值仍可通过滑块或数值输入框调整。
- **视频质量控制：** CRF 同时支持滑块和数值输入。输出分辨率可选原始尺寸、2160p、1440p、1080p、720p 和 480p，程序会根据横竖方向调整尺寸限制，保持宽高比并避免放大较小的视频。音频可选择自动编码、保留原音频或移除音频。自动编码时，音频码率可选 96、128、160、192、256 或 320 kbit/s。
- **自动并行处理：** 自动模式会把全部逻辑 CPU 核心纳入计算，再根据可用内存、编码器类型和任务数量安排并行任务数及每个 FFmpeg 进程的线程数。用户也可以将并发数量限制为 1、2、4 或 8。
- **重复处理检查：** 视频内部会写入当前编码参数标记。再次使用相同参数处理时，程序会自动跳过；ZIP 和 PPTX 还会保存处理记录。
- **输出保护：** 转换结果先写入临时文件，检查通过后再保存。默认保留已有文件，用户可以在设置中选择覆盖。
- **压缩结果统计：** 显示输入大小、输出大小和节省比例，方便检查每个视频及压缩包的实际效果。
- **任务清单：** 可以导入或导出 `.ffq` 任务清单。程序会自动保存待处理任务，重新启动后恢复上次的文件列表；已完成的输出仍会通过参数标记自动跳过。
- **记住设置：** 程序会保存上次使用的编码格式、编码器、CRF、压缩级别、音频处理方式、音频码率、输出分辨率、每个视频的裁剪范围、并发限制和输出文件夹。
- **任务控制：** 任务运行期间可以暂停、继续或取消。批量任务结束后，可以重新处理失败或取消的项目，并自动跳过已完成的文件。
- **命令行启动：** 可以把多个文件或文件夹路径直接传给程序。Windows 支持通过资源管理器的“发送到”菜单添加文件。
- **SSH 远程编码：** 自动读取本机 SSH 配置中的服务器别名，支持私钥、SSH Agent 和密码认证。密码保存在 Windows Credential Manager。远程服务器负责运行 FFmpeg，转换结果会自动下载到本机。
- **现代桌面外观：** Windows 使用 Segoe UI、Per-Monitor V2 DPI、DWM 圆角、系统标题栏配色和 Mica 背景（系统支持时），Linux 使用 Xft 抗锯齿字体。两套界面采用双列紧凑设置和队列专注视图；Linux 会按实际文字宽度缩短过长的文件名与路径。Windows 还支持清空任务列表和查看 NVENC 检测结果。

## 视频裁剪

先添加视频、文件夹、ZIP 或 PPTX。程序会在后台扫描输入文件，自动列出压缩包内的每一个视频。点击处理队列中的某个视频后，编码设置区域会显示所选视频的裁剪起止时间；可分别修改，不会影响其他视频。

在“所选视频起始时间”和“所选视频结束时间”中，可以输入秒数或 `时:分:秒.毫秒`，例如 `00:01:23.500`。起始时间默认为 `00:00:00.000`，结束时间“直到片尾”表示保留到视频结束。按 Enter 保存，点击“重置”恢复所选视频的完整时长。

点击“可视化裁剪”会显示所选视频的画面和时间轴。绿色游标表示起点，红色游标表示终点；拖动两个游标即可调整保留的片段，蓝色游标用于调整预览位置。也可以在时间轴空白处点击、使用 `<` 和 `>` 按钮或方向键移动画面位置。方向键每次移动 1 秒，按住 Shift 每次移动 10 秒。精确时间还可以直接输入。确认后点击“应用裁剪”。

打开裁剪器后，视频信息与预览画面在后台读取。对于 ZIP/PPTX，程序只会临时提取选中的内部视频进行预览，并在关闭窗口后清理临时文件。处理队列中的视频可以分别设置不同的起止时间，包括使用 SSH 服务器处理的任务。

程序在打开输入文件前传入 `-ss`，使用 FFmpeg 快速定位；需要指定结束时间时，使用输出侧 `-t` 控制片段持续时间。进度信息按照所选片段计算。输出名称和重复处理检查会记录每个视频的裁剪范围，避免不同裁剪范围的结果发生冲突。

## 任务队列操作

主窗口右上方可以切换“专注处理队列”和“返回编码设置”。专注模式会扩大处理队列的可见区域，同时保留批量进度和任务详情。队列标题旁可选择“仅看失败”，快速定位处理失败的任务，再选择“显示全部”返回完整列表。

点击任务后，右侧会显示文件来源、输出位置、处理状态以及可用的文件大小信息。任务失败时会显示原因，Windows 还可以双击任务打开详细信息。两套平台均支持使用 Delete 键移除所选任务；清除任务不会删除源文件。

常用快捷键：`Ctrl+Q` 切换专注队列视图，`Ctrl+O` 添加文件，`Ctrl+D` 清除已完成的任务，`Ctrl+Enter` 开始处理。按 Esc 可以取消当前选择。运行中的任务仍通过“暂停处理”“继续处理”和“取消任务”控制。

## 支持的文件类型

### 独立视频

支持常见视频扩展名：MP4、M4V、MOV、MKV、AVI、WebM、WMV、FLV、MPG、MPEG、3GP、TS、M2TS、MTS、VOB 和 OGV。程序会通过 FFmpeg 探测文件内容，并按设置保存为 MP4、MKV、WebM，或沿用源文件的容器格式。

### ZIP 和 PPTX

- ZIP：压缩包内的视频会按当前输出容器设置转换并替换原视频；如果扩展名发生变化，程序会同步修改常见文本文件中的路径引用。
- PPTX：程序会处理 `ppt/media` 中的视频，并更新 `ppt/slides/_rels` 中对应的 `Target`。如果文档尚未声明输出媒体格式的内容类型，程序会在 `[Content_Types].xml` 中添加声明。
- 输出文件会使用与独立视频相同的编码参数后缀，例如 `_H265_CPU_CR28_均衡_A192.pptx`。
- 同一批次中，不同目录下的同名文件会自动添加数字后缀，避免输出文件互相覆盖。
- ZIP/PPTX 的其他文件会保留。压缩包重新生成后会重新读取检查；PPTX 还会检查必要的 Office 文档结构。
- 出于安全考虑，程序会拒绝绝对路径、路径穿越和符号链接，并限制 ZIP 条目数量及展开数据量。当前限制为最多 100,000 个条目、单个文件 12 GiB、展开总量 50 GiB。

## 输出位置和文件名

默认情况下，输出文件保存在源文件所在目录。程序会追加编码格式、编码设备、质量数值、压缩级别、音频处理方式和输出分辨率，例如：

```text
holiday.mp4
holiday_H264_CPU_CR23_均衡_A192.mp4
holiday_H265_NVENC_CQ28_慢速_A192_R1080.mp4
presentation.pptx
presentation_AV1_CPU_CR30_极致压缩_A192.pptx
```

用户可以在界面中选择统一输出文件夹。处理文件夹时，默认保留相对于输入文件夹的子目录结构，以免不同目录中的同名文件互相覆盖。

## 输出容器与编码格式

| 输出容器 | 可选视频编码 |
|---|---|
| MP4 | H.264、H.265、AV1、H.266 |
| MKV | H.264、H.265、VP9、AV1、H.266 |
| WebM | VP9、AV1 |
| 原格式 | 沿用输入文件的容器，转换前检查兼容性 |

编码器按照 H.264、H.265、VP9、AV1、H.266 的年代顺序排列。VP9 使用 `libvpx-vp9`；H.266 使用 `libvvenc`，并在 FFmpeg 提供该编码器时才能选择。两者目前使用 CPU 编码。切换到与当前编码不兼容的容器时，程序会自动选择可用的兼容编码；若手动切换视频编码，也会相应调整容器。AV2 暂不作为输出编码选项。

## 竖屏视频与输出分辨率

分辨率档位限制画面的最大尺寸，输出保持原始画面方向和宽高比。系统会根据实际画面的宽高交换限制值，且不会将较小的视频放大。

| 档位 | 横屏最大尺寸 | 竖屏最大尺寸 |
|---|---|---|
| 2160p | 3840×2160 | 2160×3840 |
| 1440p | 2560×1440 | 1440×2560 |
| 1080p | 1920×1080 | 1080×1920 |
| 720p | 1280×720 | 720×1280 |
| 480p | 854×480 | 480×854 |

例如 1080×1920 的竖屏视频选择 1080p 后，仍可保持 1080×1920；选择 480p 后，输出约为 480×854。编码器可能为了满足像素格式要求，将尺寸调整到相邻的偶数。

## 编码参数

### CRF 范围

| 编码格式 | CPU 编码器 | 界面范围 | 默认值 |
|---|---|---:|---:|
| H.264 | libx264 | 0–51 | 23 |
| H.265 | libx265 | 0–51 | 28 |
| VP9 | libvpx-vp9 | 0–63 | 30 |
| AV1 | libsvtav1，或 libaom-av1 | 0–63 | 30 |
| H.266 | libvvenc（QP） | 0–63 | 32 |

CRF 数值越低，编码器通常会保留更多画面细节，输出文件也通常更大。不同编码器的质量模型不同，相同数值不能直接用于比较。

NVENC 使用 FFmpeg 的 VBR 目标质量参数 `-cq`；Quick Sync 使用 `-global_quality`；AMD AMF 使用 CQP 模式和 `-qp_i`、`-qp_p`。界面统一使用 CRF 数值输入，程序会转换为当前编码器对应的参数。

### 八档压缩级别

| 界面选项 | libx264 / libx265 | SVT-AV1 | libaom-av1 | NVENC | Quick Sync |
|---|---|---:|---:|---|---|
| 极速 | ultrafast | 13 | cpu-used 8 | p1 | veryfast |
| 非常快 | veryfast | 11 | cpu-used 7 | p2 | faster |
| 快速 | fast | 10 | cpu-used 6 | p3 | fast |
| 均衡 | medium | 8 | cpu-used 4 | p4 | medium |
| 慢速 | slow | 6 | cpu-used 3 | p5 | slow |
| 较慢 | slower | 4 | cpu-used 2 | p6 | slower |
| 很慢 | veryslow | 2 | cpu-used 1 | p7 | veryslow |
| 极致压缩 | placebo | 0 | cpu-used 0 | p7 + fullres multipass | veryslow |

AMD AMF 使用其支持的 speed、balanced、quality 三种编码质量预设，并以 CQP 参数控制画质。极致压缩模式会选择当前编码器支持的最慢高质量配置。该模式可能显著增加处理时间。

### 音频压缩

音频处理提供三种方式：自动编码会根据输出容器选择 AAC、Opus、WMA 或 MP2；保留原音频会直接复制输入音轨；移除音频会只输出视频流。自动编码时，默认码率为 192 kbit/s，界面提供 96、128、160、192、256 和 320 kbit/s 六档。保留原音频时，如果源音频编码与目标容器不兼容，FFmpeg 会报告转换失败，原始文件仍会保留。

### 自动并行调度

默认选择自动并行。CPU 编码会把全部逻辑核心纳入计算，再根据可用内存分配 FFmpeg 进程数量和每个进程的线程数。AV1 会使用更高的单任务内存估算值。NVENC、Quick Sync 和 AMF 默认最多同时运行两个硬件编码任务，以减少编码会话竞争和显存占用。

如果只添加一个 ZIP 或 PPTX，程序会根据内部视频数量安排并行转换。任务列表会显示每个内部视频的独立进度。用户可以将并发数量限制为 1、2、4 或 8；自动模式会根据实际待处理任务数再次计算。

### 重复处理标记

每个输出视频都会在容器元数据的 `comment` 字段中记录源文件指纹、编码格式、编码设备、质量数值、压缩级别、音频处理方式、音频码率和输出分辨率。程序会使用 ffprobe 读取标记；如果输入视频已经使用相同参数处理，会直接复用该文件。如果目标文件正是当前源文件使用相同参数生成的结果，则跳过转换。ZIP/PPTX 会在压缩包内保存输入文件大小、修改时间和参数指纹，用于识别相同输入的重复任务。

## 安装和运行要求

### Windows

- Windows 10 1703 或更新版本；Windows 11 可启用更多 DWM 外观效果。
- 支持 C++20 的 MSVC 工具链和 CMake 3.25 或更新版本（仅源码构建需要）。
- 安装程序和新版便携 ZIP 包均已包含 FFmpeg、ffprobe，无需额外配置。直接从源码构建时，可以使用系统安装的 FFmpeg 和 ffprobe。
- 使用 SSH 远程编码时，需要 Windows OpenSSH Client（ssh.exe、scp.exe）并确保两个程序可从 PATH 访问。
- 使用 NVENC、Quick Sync 或 AMF 时，需要对应的显卡、驱动和包含该编码器的 FFmpeg 构建。

### Linux

- 支持 C++20 的 GCC 或 Clang、CMake 3.25 或更新版本。
- X11、Xft、Fontconfig 和 libarchive。
- FFmpeg 和 ffprobe。
- 使用 SSH 远程编码时，需要安装 `openssh-client`，并确保 `ssh`、`scp` 可从 PATH 访问。
- 图形文件选择器优先使用 `zenity`，也可以使用 `kdialog`。如果两者都未安装，仍可通过命令行传入路径。

Debian 或 Ubuntu 可以安装构建依赖：

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build libx11-dev libxft-dev libfontconfig1 libarchive-dev ffmpeg fonts-noto-cjk zenity
```

## 构建

### Linux

```bash
cmake --preset linux-release
cmake --build --preset linux-release --parallel
ctest --preset linux-release
```

启动程序：

```bash
./build/linux-release/video-compression-workbench
```

### Windows

项目使用 vcpkg 安装 libarchive。安装 Visual Studio 2022 C++ 桌面开发组件和 CMake 后，在开发者 PowerShell 中执行：

```powershell
vcpkg install --triplet x64-windows-static
cmake --preset windows-release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build --preset windows-release --parallel
ctest --preset windows-release
```

生成的程序位于 `build\windows-release\video-compression-workbench.exe`。

### Windows 安装包

发布版本包含可直接安装的 NSIS 程序，以及解压即用的便携 ZIP。两种形式均包含 FFmpeg 和 ffprobe，运行时优先使用程序目录 `bin` 中的工具。安装程序默认安装到当前用户的应用目录，支持开始菜单快捷方式、可选桌面快捷方式和 Windows“已安装的应用”卸载。

从源码构建安装包，需要先安装 NSIS，然后运行：

```powershell
choco install nsis -y --no-progress
./scripts/package-windows.ps1
./scripts/test-windows-installer.ps1
```

应用 EXE 和 NSIS 安装程序的版本信息均注明版权所有者和作者 **Kenvix <i@kenvix.com>**。FFmpeg 和静态链接依赖的许可证、来源与分发说明见 [Windows 安装与第三方组件](docs/windows-distribution.md)。

## 任务清单

任务清单使用 UTF-8 文本格式，扩展名为 `.ffq`。清单记录输入路径及每个视频的裁剪范围，仍可以导入以前仅包含路径的清单。界面上方提供“导入清单”和“导出清单”按钮。导入时会按当前文件夹扫描设置添加任务，并自动去除重复路径。

## 命令行和 Windows“发送到”

可以直接把多个文件或文件夹路径传给程序。程序启动后会自动把它们加入任务列表：

```powershell
video-compression-workbench.exe "D:\Videos\clip 01.mov" "D:\Videos\clip 02.mkv" "D:\Projects\Slides.pptx"
video-compression-workbench.exe "D:\Videos\ToConvert" --output-dir "D:\Converted"
```

也可以在 Windows 资源管理器中打开 `shell:sendto`，把 `video-compression-workbench.exe` 的快捷方式放入该文件夹。之后选中一个或多个视频、ZIP 或 PPTX，使用右键菜单中的“发送到”即可自动打开程序并加入任务。

Linux 可以通过终端传入路径：

```bash
./video-compression-workbench "/home/user/Videos" "/home/user/Slides.pptx" --output-dir "/home/user/Converted"
```

## SSH 远程编码

程序可以使用本机 SSH 配置连接远程 Linux 服务器，并调用服务器上的 FFmpeg。用户仍然在本机选择文件和输出目录；程序会自动上传源视频、执行转换、读取进度、下载结果并清理远程临时文件。

1. 点击主界面右上角的“SSH 服务器”。
2. 选择 SSH 配置中的服务器，或添加新的服务器。
3. 私钥认证可以填写私钥路径，也可以留空使用 SSH Agent。密码认证会将密码保存在 Windows Credential Manager；Linux 版本在当前进程会话中使用密码。
4. 点击“测试连接”检查 SSH、FFmpeg、ffprobe 和远程编码器。
5. 点击“使用此服务器”。之后的视频、文件夹、ZIP 和 PPTX 任务都会使用该服务器的算力，结果自动回传本机。
6. 选择“本机编码”即可切回本机处理。

远程服务器需要运行 Linux 或兼容 POSIX shell 的系统，并安装 FFmpeg、ffprobe。远程硬件编码器需要相应驱动和 FFmpeg 编码器支持。详细要求、安全说明和故障排查见 [SSH 远程编码说明](docs/ssh-remote-processing.md)。
## FFmpeg 路径

默认情况下，程序会从 `PATH` 环境变量中查找 `ffmpeg` 和 `ffprobe`。如果程序安装在自定义目录，可以设置环境变量：

Linux：

```bash
export FFMPEG_PATH=/opt/ffmpeg/bin/ffmpeg
export FFPROBE_PATH=/opt/ffmpeg/bin/ffprobe
```

Windows PowerShell：

```powershell
$env:FFMPEG_PATH = 'C:\Tools\ffmpeg\bin\ffmpeg.exe'
$env:FFPROBE_PATH = 'C:\Tools\ffmpeg\bin\ffprobe.exe'
```

修改后需要重新启动程序。

## 自检和演示

列出当前 FFmpeg 构建中可用的编码器：

```bash
video-compression-workbench --encoders
```

运行本地转换、AAC 音频、参数标记、ZIP、PPTX、并行调度和进程暂停恢复测试：

```bash
video-compression-workbench --self-test
```

演示模式会保留生成的测试文件：

```bash
video-compression-workbench --demo
```

`--self-test` 会在系统临时目录中创建短视频和压缩包测试文件，并在结束时清理。测试会检查当前 FFmpeg 实际提供的 CPU 编码器；未安装的 H.264、H.265、VP9、AV1 或 H.266 编码器会跳过。ZIP、PPTX 和批量视频测试会在 H.264 CPU 编码器可用时运行。

## 自动构建和发布

GitHub Actions 在提交到 `main`、创建 Pull Request 或手动触发时构建 Windows 和 Linux 版本，并执行单元测试及实际视频、ZIP、PPTX、SSH 测试。`main` 上两个平台的全部测试通过后，发布任务读取 `CMakeLists.txt` 中的版本号。如果该版本尚未发布，就从通过测试的提交创建版本标签和 GitHub Release，并上传包含 FFmpeg 的 Windows NSIS 安装程序、Windows 便携 ZIP 包和 Linux tar.gz 包。已经发布的版本不会被后续提交覆盖；发布下一版本时，需要先更新 CMake 项目版本号。手动推送与项目版本一致的 `v*` 标签，也可以触发相同的验证和发布流程。

## 维护要求

修改编码参数时，应同步检查 `src/core/conversion.cpp`、`tests/core_tests.cpp` 和 `docs/encoding.md`。修改 ZIP/PPTX 处理时，应同步检查 `src/core/archive.cpp` 和 `docs/architecture.md`。修改任务调度时，应检查 `src/core/app_controller.cpp` 和并行调度测试。修改界面时，应同时检查 Windows 和 Linux 两套实现。

提交代码前至少执行：

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/video-compression-workbench --self-test
```

项目决策和后续维护要求记录在 [`docs/decision-log.md`](docs/decision-log.md)。

产品功能评估和后续开发建议见 [`docs/product-review.md`](docs/product-review.md)。

新开发者请先阅读[开发者接管指南](docs/developer-handoff.md)，其中包含构建环境、目录说明、实现约束、测试流程和发布方法。
