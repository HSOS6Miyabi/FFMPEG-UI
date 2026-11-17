# FFmpeg Studio

FFmpeg Studio 是一款面向 Windows 和 Linux 的本地视频压缩工具，支持单文件、批量文件、文件夹、ZIP 压缩包和 PowerPoint 演示文稿。可以统一输出 MP4，也可以让每个视频沿用源文件的容器格式；支持 H.264、H.265 或 AV1，并通过 CRF 数值和压缩级别控制画质与文件大小。

程序使用 C++20 编写。Windows 界面使用 Win32 API 和 GDI，Linux 界面使用 X11 和 Xft。程序通过 FFmpeg 和 ffprobe 完成视频编码，通过 libarchive 读取和生成 ZIP 文件。项目不使用 Qt、GTK、MFC、Electron 等桌面界面框架，视频文件也不会上传到网络。

## 功能概览

- **多种输入方式：** 添加单个或多个视频、ZIP、PPTX，也可以添加整个文件夹。支持将文件直接拖入窗口。
- **批量任务队列：** 每个视频单独显示状态和进度；ZIP、PPTX 会列出内部视频的处理状态。右侧显示全部任务的总体进度。
- **文件夹扫描：** 默认递归扫描子文件夹，可关闭递归扫描。指定输出文件夹后，可以保留原有目录结构。
- **ZIP 处理：** 展开 ZIP，扫描其中的视频，按照所选输出容器转换后替换原视频并重新生成 ZIP。其他文件和目录会保留，常见文本、XML 和网页文件中的视频路径也会随文件名变化而更新。
- **PPTX 处理：** 读取 PPTX 内部 ZIP 包，逐个转换嵌入视频，更新幻灯片关系文件和对应的媒体内容类型，再生成新的 PPTX。原始演示文稿始终保留。
- **三家硬件编码器：** 自动检测 NVIDIA NVENC、Intel Quick Sync 和 AMD AMF。程序会检查 FFmpeg 编码器列表，并执行单帧测试；当前设备无法实际使用的编码器不会显示为可选项。
- **八档压缩级别：** 从极速到极致压缩，适合快速预览、日常使用和长时间归档。CRF 数值既可以通过滑块调整，也可以直接输入。
- **视频质量控制：** CRF 同时支持滑块和数值输入。音频码率可独立选择 96、128、160、192、256 或 320 kbit/s；程序会按输出容器选择 AAC、Opus、WMA 或 MP2。
- **自动并行处理：** 自动模式会把全部逻辑 CPU 核心纳入计算，再根据可用内存、编码器类型和任务数量安排并行任务数及每个 FFmpeg 进程的线程数。用户也可以将并发数量限制为 1、2、4 或 8。
- **重复处理检查：** 视频内部会写入当前编码参数标记。再次使用相同参数处理时，程序会自动跳过；ZIP 和 PPTX 还会保存处理记录。
- **输出保护：** 转换结果先写入临时文件，检查通过后再保存。默认保留已有文件，用户可以在设置中选择覆盖。
- **压缩结果统计：** 显示输入大小、输出大小和节省比例，方便检查每个视频及压缩包的实际效果。
- **任务清单：** 可以导入或导出 `.ffq` 任务清单，保存待处理文件列表，方便分批执行和重复使用。
- **记住设置：** 程序会保存上次使用的编码格式、编码器、CRF、压缩级别、音频码率、并发限制和输出文件夹。
- **任务控制：** 任务运行期间可以暂停、继续或取消。批量任务结束后，可以重新处理失败或取消的项目，并自动跳过已完成的文件。
- **命令行启动：** 可以把多个文件或文件夹路径直接传给程序。Windows 支持通过资源管理器的“发送到”菜单添加文件。
- **现代桌面外观：** Windows 使用 Segoe UI、Per-Monitor V2 DPI、DWM 圆角、系统标题栏配色和 Mica 背景（系统支持时）。Linux 使用 Xft 抗锯齿字体，并根据显示缩放调整布局。

## 支持的文件类型

### 独立视频

支持常见视频扩展名：MP4、M4V、MOV、MKV、AVI、WebM、WMV、FLV、MPG、MPEG、3GP、TS、M2TS、MTS、VOB 和 OGV。程序会通过 FFmpeg 探测文件内容，并按设置保存为 MP4 或沿用源文件的容器格式。

### ZIP 和 PPTX

- ZIP：压缩包内的视频会按当前输出容器设置转换。原视频文件会由压缩后的 MP4 替换；如果扩展名发生变化，程序会同步修改常见文本文件中的路径引用。
- PPTX：程序会处理 `ppt/media` 中的视频，并更新 `ppt/slides/_rels` 中对应的 `Target`。如果文档尚未声明输出媒体格式的内容类型，程序会在 `[Content_Types].xml` 中添加声明。
- 输出文件会使用与独立视频相同的编码参数后缀，例如 `_H265_CPU_CR28_均衡_A192.pptx`。
- 同一批次中，不同目录下的同名文件会自动添加数字后缀，避免输出文件互相覆盖。
- ZIP/PPTX 的其他文件会保留。压缩包重新生成后会重新读取检查；PPTX 还会检查必要的 Office 文档结构。
- 出于安全考虑，程序会拒绝绝对路径、路径穿越和符号链接，并限制 ZIP 条目数量及展开数据量。当前限制为最多 100,000 个条目、单个文件 12 GiB、展开总量 50 GiB。

## 输出位置和文件名

默认情况下，输出文件保存在源文件所在目录。程序会追加编码格式、编码设备、质量数值、音频码率和压缩级别，例如：

```text
holiday.mp4
holiday_H264_CPU_CR23_均衡_A192.mp4
holiday_H265_NVENC_CQ28_慢速_A192.mp4
presentation.pptx
presentation_AV1_CPU_CR30_极致压缩_A192.pptx
```

用户可以在界面中选择统一输出文件夹。处理文件夹时，默认保留相对于输入文件夹的子目录结构，以免不同目录中的同名文件互相覆盖。

## 编码参数

### CRF 范围

| 编码格式 | CPU 编码器 | 界面范围 | 默认值 |
|---|---|---:|---:|
| H.264 | libx264 | 0–51 | 23 |
| H.265 | libx265 | 0–51 | 28 |
| AV1 | libsvtav1，或 libaom-av1 | 0–63 | 30 |

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

输入音轨会转换为 AAC 有损音频。默认码率为 192 kbit/s，界面提供 96、128、160、192、256 和 320 kbit/s 六档。没有音轨的视频不会生成额外音轨。音频码率与视频 CRF 分别设置。

### 自动并行调度

默认选择自动并行。CPU 编码会把全部逻辑核心纳入计算，再根据可用内存分配 FFmpeg 进程数量和每个进程的线程数。AV1 会使用更高的单任务内存估算值。NVENC、Quick Sync 和 AMF 默认最多同时运行两个硬件编码任务，以减少编码会话竞争和显存占用。

如果只添加一个 ZIP 或 PPTX，程序会根据内部视频数量安排并行转换。任务列表会显示每个内部视频的独立进度。用户可以将并发数量限制为 1、2、4 或 8；自动模式会根据实际待处理任务数再次计算。

### 重复处理标记

每个输出视频都会在容器元数据的 `comment` 字段中记录源文件指纹、编码格式、编码设备、质量数值、压缩级别和音频码率。程序会使用 ffprobe 读取标记；如果输入视频已经使用相同参数处理，会直接复用该文件。如果目标文件正是当前源文件使用相同参数生成的结果，则跳过转换。ZIP/PPTX 会在压缩包内保存输入文件大小、修改时间和参数指纹，用于识别相同输入的重复任务。

## 安装和运行要求

### Windows

- Windows 10 1703 或更新版本；Windows 11 可启用更多 DWM 外观效果。
- 支持 C++20 的 MSVC 工具链和 CMake 3.25 或更新版本（仅源码构建需要）。
- FFmpeg 和 ffprobe，并且两个程序都能从命令行访问。
- 使用 NVENC、Quick Sync 或 AMF 时，需要对应的显卡、驱动和包含该编码器的 FFmpeg 构建。

### Linux

- 支持 C++20 的 GCC 或 Clang、CMake 3.25 或更新版本。
- X11、Xft、Fontconfig 和 libarchive。
- FFmpeg 和 ffprobe。
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
./build/linux-release/ffmpeg-ui
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

生成的程序位于 `build\windows-release\Release\ffmpeg-ui.exe`。

## 任务清单

任务清单使用 UTF-8 文本格式，扩展名为 `.ffq`。每行保存一个视频、ZIP、PPTX 或文件夹路径。界面上方提供“导入清单”和“导出清单”按钮。导入时会按当前文件夹扫描设置添加任务，并自动去除重复路径。

## 命令行和 Windows“发送到”

可以直接把多个文件或文件夹路径传给程序。程序启动后会自动把它们加入任务列表：

```powershell
ffmpeg-ui.exe "D:\Videos\clip 01.mov" "D:\Videos\clip 02.mkv" "D:\Projects\Slides.pptx"
ffmpeg-ui.exe "D:\Videos\ToConvert" --output-dir "D:\Converted"
```

也可以在 Windows 资源管理器中打开 `shell:sendto`，把 `ffmpeg-ui.exe` 的快捷方式放入该文件夹。之后选中一个或多个视频、ZIP 或 PPTX，使用右键菜单中的“发送到”即可自动打开程序并加入任务。

Linux 可以通过终端传入路径：

```bash
./ffmpeg-ui "/home/user/Videos" "/home/user/Slides.pptx" --output-dir "/home/user/Converted"
```

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
ffmpeg-ui --encoders
```

运行本地转换、AAC 音频、参数标记、ZIP、PPTX、并行调度和进程暂停恢复测试：

```bash
ffmpeg-ui --self-test
```

演示模式会保留生成的测试文件：

```bash
ffmpeg-ui --demo
```

`--self-test` 会在系统临时目录中创建测试文件，并在结束时清理。测试需要本机 FFmpeg 包含 CPU H.264、H.265 和 AV1 编码器。

## 自动构建和发布

GitHub Actions 会在提交到 `main`、创建 Pull Request 或手动触发时构建 Windows 和 Linux 版本，并执行单元测试及实际视频、ZIP、PPTX 测试。推送符合 `v*` 格式的标签（例如 `v1.2.0`）后，工作流会自动创建 GitHub Release，并附上 Windows ZIP 包和 Linux tar.gz 包。

## 维护要求

修改编码参数时，应同步检查 `src/core/conversion.cpp`、`tests/core_tests.cpp` 和 `docs/encoding.md`。修改 ZIP/PPTX 处理时，应同步检查 `src/core/archive.cpp` 和 `docs/architecture.md`。修改任务调度时，应检查 `src/core/app_controller.cpp` 和并行调度测试。修改界面时，应同时检查 Windows 和 Linux 两套实现。

提交代码前至少执行：

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/ffmpeg-ui --self-test
```

项目决策和后续维护要求记录在 [`docs/decision-log.md`](docs/decision-log.md)。

新开发者请先阅读[开发者接管指南](docs/developer-handoff.md)，其中包含构建环境、目录说明、实现约束、测试流程和发布方法。
