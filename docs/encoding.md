# 编码参数说明

## 质量参数

CPU 编码使用各编码器的恒定质量参数：

- H.264：`libx264 -crf N`
- H.265：`libx265 -crf N`
- SVT-AV1：`libsvtav1 -crf N`
- libaom AV1：`libaom-av1 -crf N -b:v 0`

H.264 和 H.265 的界面范围为 0–51，AV1 的界面范围为 0–63。不同编码器使用不同的量化模型，因此相同数值不代表相同画质或文件大小。

硬件编码器使用各自支持的质量参数：

- NVENC：`-rc vbr -cq N -b:v 0`
- Intel Quick Sync：`-global_quality N`
- AMD AMF：`-rc cqp -qp_i N -qp_p N`

界面统一提供 CRF 滑块和数值输入框，命令生成器会将数值转换为所选编码器支持的参数。

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

程序根据目标容器选择音频编码器：MP4、MOV、MKV、AVI 等常见容器使用 AAC，WebM 使用 Opus，WMV/ASF 使用 WMA，MPEG 程序流使用 MP2。音频码率可选 96、128、160、192、256 和 320 kbit/s，默认值为 192 kbit/s。没有音轨的视频不会生成额外音轨。音频码率独立于视频 CRF 和压缩级别。

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
| AV1 | `libsvtav1`，缺少时使用 `libaom-av1` |

硬件编码器：

| 厂商 | H.264 | H.265 | AV1 |
|---|---|---|---|
| NVIDIA NVENC | `h264_nvenc` | `hevc_nvenc` | `av1_nvenc` |
| Intel Quick Sync | `h264_qsv` | `hevc_qsv` | `av1_qsv` |
| AMD AMF | `h264_amf` | `hevc_amf` | `av1_amf` |

程序启动时读取 FFmpeg 编码器列表，并对每个硬件编码器执行一帧测试。只有测试成功的编码器才会显示为可选项。不同显卡型号、驱动和 FFmpeg 构建可能支持不同的编码格式。

## 输出容器格式

视频任务提供两种输出容器模式：

- **MP4**：所有视频统一输出为 `.mp4`，默认使用 MP4 容器和 `+faststart`。
- **与源文件相同**：每个视频沿用源文件的扩展名和容器格式。例如 MKV 输入仍输出 MKV，MOV 输入仍输出 MOV。ZIP/PPTX 内的视频也使用相同规则，容器中的媒体引用会在扩展名发生变化时同步更新。

当前支持识别的源容器包括 MP4、M4V、MOV、MKV、AVI、WebM、WMV、FLV、MPEG、3GP、MPEG-TS、VOB 和 OGV。程序会检查所选视频编码与目标容器是否兼容。WebM、MPEG-PS 等容器对视频和音频编码有额外限制；如果编码组合不受容器支持，任务会显示错误并保留原始文件。

相同容器模式仍会重新编码视频和音频，CRF、压缩级别及音频码率继续生效。它只决定输出封装格式，不代表无损复制。音频编码器会按目标容器选择：WebM 使用 Opus，WMV/ASF 使用 WMA，MPEG 程序流使用 MP2，其余常见容器使用 AAC。重复处理标记包含实际容器扩展名。

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

程序转换第一个视频流，并保留全部音频流。音频统一编码为 AAC，目标码率由界面选择，默认值为 192 kbit/s。输出像素格式为 8-bit `yuv420p`，以提高常见播放器的兼容性。10-bit 输入会转换为 8-bit。

字幕流和数据流默认不复制。H.265 输出使用 `hvc1` 标记，AV1 输出使用 `av01` 标记。

## 重复处理标记

程序会在输出媒体文件的 `comment` 元数据字段中写入源文件指纹，以及编码格式、编码设备、质量数值、压缩级别、音频码率和输出容器模式。重复处理时，程序使用 ffprobe 读取该标记。参数相同时，程序会直接复用已处理的视频文件；如果目标文件已经是当前源文件使用相同参数生成的输出文件，则直接跳过写入。

ZIP/PPTX 内会额外保存输入文件大小、修改时间和参数指纹。重新处理同一输入时，如果处理记录与当前文件和参数完全相同，程序会跳过整个压缩包任务。
