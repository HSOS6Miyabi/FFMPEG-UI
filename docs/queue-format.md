# 任务清单格式与恢复规则

本文件说明用户配置目录中 `queue.ffq` 的数据结构、旧版兼容方式，以及修改任务保存代码时需要遵守的规则。负责实现的文件是 `src/core/app_controller.cpp`。

## 文件位置和用途

| 平台 | 设置目录 | 文件 |
|---|---|---|
| Windows | `%APPDATA%\FFmpegStudio` | `settings.ini`、`queue.ffq` |
| Linux | `$XDG_CONFIG_HOME/ffmpeg-studio` 或 `~/.config/ffmpeg-studio` | `settings.ini`、`queue.ffq` |
| 自动化测试 | `FFMPEG_STUDIO_CONFIG_DIR` 指定的目录 | `settings.ini`、`queue.ffq` |

`settings.ini` 保存编码格式、设备、质量、压缩级别、音频、输出目录等批次设置。各视频独立的裁剪范围保存在 `queue.ffq` 的视频项目中。设置文件仍保留历史裁剪字段，现行任务裁剪时间以各视频项目的设置为准。

## 版本 2 的内容

文件使用 UTF-8 文本，每一行是一条记录：

- 第一行固定为 `FFmpegStudioQueue/2`。
- `T "路径"` 表示一个输入视频或 ZIP/PPTX 任务。字符串使用 C++ `std::quoted` 格式。
- `I "内部视频名称" 起始秒数 结束秒数` 表示此前最近一个 `T` 任务中某个视频的裁剪范围。数字单位为秒。
- 独立视频任务的内部名称是空字符串 `""`；ZIP/PPTX 使用包内相对路径，例如 `"ppt/media/clip.mp4"`。
- 完整视频的范围默认为起始 `0`、结束 `-1`。默认范围不需要写 `I` 记录。
- `-1` 表示保留至视频结束；非负结束时间必须晚于起始时间。

例如：

~~~text
FFmpegStudioQueue/2
T "/media/videos/example.mp4"
I "" 1.25 4.75
T "/media/slides/demo.pptx"
I "ppt/media/clip.mp4" 3 8.5
T "/media/videos/second.mkv"
~~~

以上数据表示：第一个视频只保留 1.25 至 4.75 秒，PPTX 中的 `ppt/media/clip.mp4` 保留 3 至 8.5 秒，其他视频完整处理。

## 读取、写入与兼容性

- `AppController::loadQueue` 在启动时读取保存的清单，随后把路径交给后台扫描线程重新建立任务。扫描结束之前，不能据此判断恢复已经完成。
- `AppController::saveQueueLocked` 在保存前先写临时文件，再替换 `queue.ffq`。已完成和已跳过的任务不会进入自动恢复清单。
- `AppController::exportQueue` 导出当前队列中的任务，包括已完成的任务及已设置的逐视频裁剪范围。
- `AppController::importQueue` 接受两个版本。返回值是从文件中读取的任务路径数量，不应直接当作“实际新增任务数”；还要考虑输入路径去重及后台扫描时间。
- `FFmpegStudioQueue/1` 文件从第二行起每行只包含一个路径；导入时仍可使用。版本 2 由 `T` 和 `I` 行表示路径和逐视频设置。
- 任务状态、即时进度、计算后的输出路径没有保存在清单中。重新启动时重新建立待处理任务，并根据当前编码设置重新计算输出路径。已有输出的重复处理检查由 FFmpeg 媒体标记和压缩包处理记录完成。
- 移动源文件或压缩包内部改名后，旧路径无法自动对应新文件。用户需要重新选择输入文件并核对相应的裁剪范围。

## 修改保存规则时的测试

1. 创建两个独立视频，分别修改不同裁剪范围，导出后重新读取，检查二者没有互相影响。
2. 为一个 ZIP/PPTX 的多个内部视频分别设置范围，关闭程序、重新打开，确认内部相对路径对应正确。
3. 导入版本 1 清单和版本 2 清单，检查路径去重、中文文件名、缺省裁剪值。
4. 修改编码设置后，验证输出路径重新计算、重复处理标记正确，原始媒体保持不变。
5. 在扫描期间关闭应用，确认不会写入损坏的清单；再启动后检查已保存任务。
6. 执行 `ctest` 与 `--self-test`，并在 Windows、Linux 界面中检查任务状态和文件列表。

维护队列格式时，还需要同步更新[程序结构](architecture.md)、[开发者接管指南](developer-handoff.md)和[维护台账](maintenance-ledger.md)。
