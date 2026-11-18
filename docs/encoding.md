# 编码参数说明

## 质量参数

CPU 编码使用各编码器的恒定质量参数：

- H.264：`libx264 -crf N`
- H.265：`libx265 -crf N`
- VP9：`libvpx-vp9 -crf N -b:v 0`（按档位调整 `-cpu-used`）
- H.266：`libvvenc -qp N`（需要 FFmpeg 启用 libvvenc，使用 10-bit 输入像素格式）
- SVT-AV1：`libsvtav1 -crf N`
- libaom AV1：`libaom-av1 -crf N -b:v 0`

H.264 和 H.265 的界面范围为 0–51，VP9、AV1 和 H.266 的界面范围为 0–63；H.266 的数值对应 QP。不同编码器使用不同的量化模型，因此相同数值不代表相同画质或文件大小。

硬件编码器使用各自支持的质量参数：

- NVENC：`-rc vbr -cq N -b:v 0`
- Intel Quick Sync：`-global_quality N`
- AMD AMF：`-rc cqp -qp_i N -qp_p N`

界面提供质量滑块和数值输入框，命令生成器会将数值转换为所选编码器支持的 CRF、QP 或硬件质量参数。

## 八档压缩级别

档位越慢，编码器通常会花费更多计算时间寻找更高的压缩效率。相同质量数值下，较慢档位通常能得到更小的文件，但实际结果还取决于视频内容和编码器实现。

| 级别 | x264 / x265 preset | SVT-AV1 preset | libaom cpu-used | NVENC preset | Quick Sync preset |
|---|---|---:|---:|---|---|
| 极速 | ultrafast | 13 | 8 | p1 | veryfast |
| 非常快 | veryfast | 11 | 7 | p2 | faster |
| 快速 | fast | 10 | 6 | p3 | fast |
| 均衡 | medium | 8 | 4 | p4 | medium |
| 慢速 | slow | 6 | 3 | p5 | slow |
| 较慢 | slower | 4 | 2 | p6 | slower |
| 很慢 | veryslow | 2 | 1 | p7 | veryslow |
| 极致压缩 | placebo | 0 | 0 | p7 + fullres multipass | veryslow |

VP9 使用 `-deadline good`、`-row-mt 1` 和 `-cpu-used`，八档从 8 逐步降至 0。H.266 使用 libvvenc 的 `faster`、`fast`、`medium`、`slow`、`slower` 五种可用预设，其中部分相邻档位共享预设。

AMD AMF 使用其支持的 `speed`、`balanced`、`quality` 预设，并使用 CQP 参数控制画质。AMF 的预设数量由其编码器接口决定，因此多个相邻档位会映射到相同的 AMF 质量预设。

极致压缩模式会选择当前编码器支持的最慢高质量配置：

- x264：`-preset placebo`
- x265：`-preset placebo`
- SVT-AV1：`-preset 0`
- libaom AV1：`-cpu-used 0`
- NVENC：`-preset p7 -tune hq -multipass fullres`
- Quick Sync：`-preset veryslow -look_ahead 1`

该模式可能显著增加编码时间，适合不需要关注转换速度的归档任务。

## 音频压缩

程序根据输出扩展名选择音频编码器：WebM 优先使用 Opus；如果 FFmpeg 未提供 libopus，则使用 Vorbis。WMV 使用 WMA，MPG、MPEG 和 VOB 使用 MP2，其他支持的容器使用 AAC。音频码率可选 96、128、160、192、256 和 320 kbit/s，默认值为 192 kbit/s。没有音轨的视频不会生成额外音轨。音频码率独立于视频 CRF 和压缩级别。

## 音频处理方式

界面提供三种音频处理方式：

- **自动编码**：根据目标容器选择 AAC、Opus、WMA 或 MP2，并使用界面选择的音频码率。
- **保留原音频**：使用 FFmpeg 的 -c:a copy 选项直接复制源音轨，不重新编码，也不应用音频码率设置。目标容器不支持源音频编码时，FFmpeg 会报告转换失败，程序会保留原始文件。
- **移除音频**：只映射视频流，不输出音频流。此模式不会调用音频编码器。

三个模式都会写入不同的处理参数标记。输出文件名也会分别使用 _A192、_ACOPY 或 _ANOAUDIO 等后缀。

## 输出分辨率

界面有“原始尺寸”和五个分辨率限制档位，按照实际画面方向选择宽高限制：

| 档位 | 横屏最大宽度×高度 | 竖屏最大宽度×高度 |
|---|---|---|
| 原始尺寸 | 原始尺寸 | 原始尺寸 |
| 2160p | 3840×2160 | 2160×3840 |
| 1440p | 2560×1440 | 1440×2560 |
| 1080p | 1920×1080 | 1080×1920 |
| 720p | 1280×720 | 720×1280 |
| 480p | 854×480 | 480×854 |

FFmpeg 的 scale 滤镜先根据输入画面的 `iw` 与 `ih` 判断方向，再选择对应限制，并使用 `force_original_aspect_ratio=decrease` 保持原始比例。程序不会放大较小的源视频，输出尺寸将调整为偶数。竖屏视频保持竖屏方向，例如 1080×1920 的视频选择 1080p 时可保留原始尺寸，选择 480p 时输出约 480×854。

分辨率设置参与输出文件命名和重复处理检查，ZIP/PPTX 内的视频使用同一计算规则。

## 自动并行调度

程序根据逻辑 CPU 核心数、可用物理内存、编码器类型和待处理项目数量计算并行任务数。

- CPU H.264/H.265 以每个任务约 2 GiB 内存估算。
- CPU AV1 以每个任务约 3 GiB 内存估算。
- NVENC、Quick Sync 和 AMF 以每个任务约 1.5 GiB 内存估算，并默认最多并行两个硬件编码任务。
- 自动模式会把全部逻辑核心纳入计算，再根据可用内存和待处理文件数量分配并行任务数。
- 每个 FFmpeg 进程都会收到 `-threads:v` 参数。程序使用向上取整分配线程数，尽量让总线程数覆盖全部逻辑核心。
- 用户可以将并发任务数限制为 1、2、4 或 8。

当一个 ZIP 或 PPTX 是唯一的顶层任务时，程序会根据内部视频数量安排并行转换。多个顶层任务同时运行时，压缩包内部转换会使用单个工作线程，以避免嵌套并发导致过度占用资源。用户可以在任务运行期间暂停编码、继续处理或取消任务。

## 实时进度

程序使用 FFmpeg 的 `-progress pipe:1` 接收结构化进度信息，并通过 `-stats_period 0.25` 设置更新间隔。进度数据包括：

- `frame`：已处理帧数。
- `fps`：当前处理帧率。
- `out_time_us`：当前媒体处理时间，单位为微秒。
- `speed`：编码速度相对于实时播放速度的倍率。
- `bitrate`：当前输出码率。
- `total_size`：当前已经写入的字节数。

程序使用 ffprobe 读取源视频总时长，根据当前处理时间计算百分比，并根据剩余媒体时长和 `speed` 计算预计剩余时间。

## 编码器选择

CPU 编码器：

| 输出格式 | FFmpeg 编码器 |
|---|---|
| H.264 | `libx264` |
| H.265 | `libx265` |
| VP9 | `libvpx-vp9` |
| H.266 | `libvvenc`，在 FFmpeg 启用该库时可用 |
| AV1 | `libsvtav1`，缺少时使用 `libaom-av1` |

硬件编码器：

| 厂商 | H.264 | H.265 | AV1 |
|---|---|---|---|
| NVIDIA NVENC | `h264_nvenc` | `hevc_nvenc` | `av1_nvenc` |
| Intel Quick Sync | `h264_qsv` | `hevc_qsv` | `av1_qsv` |
| AMD AMF | `h264_amf` | `hevc_amf` | `av1_amf` |

程序启动时读取 FFmpeg 编码器列表，并对各硬件编码器执行短片段编码测试。VP9 与 H.266 使用 CPU 编码，当前 FFmpeg 缺少相关库时，对应选项会显示为不可用。只有测试成功的编码器才会显示为可选项。不同显卡型号、驱动和 FFmpeg 构建可能支持不同的编码格式。

## 输出容器格式

视频任务提供四种输出容器模式：

- **MP4**：所有视频统一输出为 `.mp4`，使用 MP4 容器和 `+faststart`。
- **MKV**：统一输出为 `.mkv`，兼容 H.264、H.265、VP9、AV1 和 H.266。
- **WebM**：统一输出为 `.webm`，使用 VP9 或 AV1，并优先使用 Opus 音频。
- **与源文件相同**：每个视频沿用源文件的扩展名和容器格式。例如 MKV 输入仍输出 MKV，MOV 输入仍输出 MOV。ZIP/PPTX 内的视频也使用相同规则，容器中的媒体引用会在扩展名发生变化时同步更新。

当前支持识别的源容器包括 MP4、M4V、MOV、MKV、AVI、WebM、WMV、FLV、MPEG、3GP、MPEG-TS、VOB 和 OGV。程序会检查所选视频编码与目标容器是否兼容。WebM、MPEG-PS 等容器对视频和音频编码有额外限制；如果编码组合不受容器支持，任务会显示错误并保留原始文件。

相同容器模式会重新编码视频，并按照所选音频处理方式处理音轨。自动编码模式会根据目标容器选择音频编码器；保留原音频模式会复制源音轨；移除音频模式会省略音轨。分辨率设置、音频处理方式和实际容器扩展名都会写入重复处理标记。

MP4 输出参数包括：

```text
-map 0:v:0
-map 0:a?
-map_metadata 0
-map_chapters 0
-pix_fmt yuv420p
-c:a aac -b:a 192k
-movflags +faststart
```

程序转换第一个视频流，并根据音频处理方式映射音轨。自动编码模式通常使用 AAC，WebM 使用 Opus 或 Vorbis，目标码率由界面选择；保留原音频模式使用 `-c:a copy`；移除音频模式不映射音轨。常见编码使用 8-bit `yuv420p`，H.266 使用 libvvenc 所需的 10-bit `yuv420p10le`。

字幕流和数据流默认不复制。H.265 输出使用 `hvc1` 标记，AV1 输出使用 `av01` 标记。

## 重复处理标记

程序会在输出媒体文件的 `comment` 元数据字段中写入源文件指纹，以及编码格式、编码设备、质量数值、压缩级别、音频处理方式、音频码率、输出分辨率和输出容器模式。重复处理时，程序使用 ffprobe 读取该标记。参数相同时，程序会直接复用已处理的视频文件；如果目标文件已经是当前源文件使用相同参数生成的输出文件，则直接跳过写入。

ZIP/PPTX 内会额外保存输入文件大小、修改时间和参数指纹。重新处理同一输入时，如果处理记录与当前文件和参数完全相同，程序会跳过整个压缩包任务。
