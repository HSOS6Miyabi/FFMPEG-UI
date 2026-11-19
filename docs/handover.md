# 接管与维护手册

本手册面向首次接手视频压缩工作台的开发者。建议按顺序完成环境准备、阅读代码、运行测试、检查桌面界面，再开始修改功能。项目现状见[开发状态记录](development-checkpoint.md)，重要修改见[维护台账](maintenance-ledger.md)。

## 接管前先确认

1. 阅读仓库根目录的 [README](../README.md)，了解使用方式、支持的编码器和文件类型。
2. 阅读[程序结构](architecture.md)和[项目决策记录](decision-log.md)，了解共享核心、双平台界面和不可忽略的文件安全规则。
3. 阅读[开发者接管指南](developer-handoff.md)、[编码参数说明](encoding.md)和[SSH 远程编码](ssh-remote-processing.md)，确定准备修改的模块与已有测试。
4. 阅读[构建、测试与发布手册](ci-release.md)，确认本机工具、GitHub Actions、安装程序和正式发布的关系。
5. 检查 `main` 的最新提交和 GitHub Actions 结果。检查 [GitHub Release](https://github.com/HSOS6Miyabi/FFMPEG-UI/releases) 的标签与提交位置，不要单凭 `CMakeLists.txt` 中的版本号判断下载包包含哪些修改。

## 代码位置和职责

| 要修改的功能 | 主要文件 | 需要同时检查 |
|---|---|---|
| FFmpeg 编码命令、质量和容器兼容性 | `src/core/conversion.cpp`、`conversion.hpp` | `tests/core_tests.cpp`、`docs/encoding.md`、实际媒体自检 |
| 任务导入、后台扫描、状态、保存与恢复 | `src/core/app_controller.cpp`、`app_controller.hpp` | 两套界面、`queue.ffq` 格式、异步扫描测试 |
| ZIP/PPTX 内部文件、媒体替换和安全限制 | `src/core/archive.cpp`、`archive.hpp` | 内部路径、引用文件、中文路径、ZIP/PPTX 自检 |
| 画面预览、压缩包视频临时提取 | `src/core/preview.cpp`、`preview.hpp` | 后台线程、临时文件、游标事件及两套界面 |
| Windows 原生窗口 | `src/platform/windows_ui.cpp` | DPI、鼠标点击区域、`scripts/test-windows-ui.ps1` |
| Linux 原生窗口 | `src/platform/linux_ui.cpp` | Xft、窗口缩放、Xdnd、`.github/workflows/build.yml` 中的截图脚本 |
| 进程运行、暂停、继续和取消 | `src/core/process.cpp`、`process.hpp` | Windows 进程及 Job Object、Linux 进程组 |
| SSH 凭据和远程执行 | `src/core/ssh.cpp`、`ssh.hpp` | 认证安全、临时目录及 SSH 集成测试 |
| 构建与分发 | `CMakeLists.txt`、`CMakePresets.json`、`.github/workflows/build.yml` | Windows 打包脚本、NSIS、Linux tar.gz、第三方许可证 |

## 最小接管验证

**Linux 开发环境**

~~~bash
cmake --preset linux-release
cmake --build --preset linux-release --parallel
ctest --preset linux-release
./build/linux-release/video-compression-workbench --self-test
~~~

构建依赖和图形测试额外软件见[构建、测试与发布手册](ci-release.md)。桌面程序需要 X11 显示环境；在无桌面服务器中运行界面测试时使用 Xvfb。

**Windows 开发环境**

在已配置 Visual Studio C++ 编译工具、CMake、Ninja 与 vcpkg 的 PowerShell 中运行：

~~~powershell
vcpkg install --triplet x64-windows-static
cmake --preset windows-release -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build --preset windows-release --parallel
ctest --preset windows-release
.\build\windows-release\video-compression-workbench.exe --self-test
~~~

执行实际界面操作前，还应确认 FFmpeg、ffprobe 可使用；Windows 正式安装包自带这两项工具，源码构建可以使用系统工具。

## 启动后要检查的交互

1. 拖入两个独立视频，确认队列中可分别选中、编辑时间，修改一个视频的裁剪范围后另一个视频仍保持自己的时间。
2. 拖入包含多个视频的 ZIP、PPTX，确认后台扫描时界面持续响应，内部视频会逐步出现；选中一个内部视频后可打开裁剪器。
3. 拖入带有子文件夹的目录，确认按“递归扫描”设置加入视频，以及文件路径去重、输出目录结构保存。
4. 打开裁剪器，检查绿色起点、红色终点、蓝色预览游标可以分别操作；等待解码期间可继续使用界面。
5. 选择较慢的编码档位，检查开始、暂停、继续、取消、状态更新、输出文件验证与再次运行时的重复处理检查。
6. 在 Windows/Linux 两端检查普通尺寸和缩小窗口、不同 DPI、深浅色主题，以及含中文的长文件名。
7. 导出任务清单、退出程序、重新启动并恢复，确认各视频的裁剪范围仍然存在。

以上交互中，耗时操作应在后台进行，界面应显示进展信息。编码器探测和文件扫描未结束前，开始批量处理会受到限制。

## 用户数据与兼容性

- 设置和自动恢复队列保存在当前用户的配置目录。Windows 为 `%APPDATA%\FFmpegStudio`，Linux 为 `$XDG_CONFIG_HOME/ffmpeg-studio`；没有指定 `XDG_CONFIG_HOME` 时使用 `~/.config/ffmpeg-studio`。测试可以通过 `FFMPEG_STUDIO_CONFIG_DIR` 指定独立目录。
- `settings.ini` 保存整个批次共用的编码选项；每个视频的裁剪时间保存在 `queue.ffq` 中。详情见[程序结构](architecture.md)与[任务清单格式](queue-format.md)。
- 修改配置读取逻辑时，旧配置缺少新字段也必须能正常启动。修改队列格式时，保留对版本 1 的读取支持，并用单元测试验证恢复与导入。
- 处理用户文件前，保留输出文件检查和临时文件机制。不得让失败或取消的任务覆盖原始媒体。
- 不得将 SSH 密码、私钥内容、临时认证信息提交到仓库或写入公开日志。检查调试日志与截图时同时注意用户文件路径和服务器信息。
- 修改代码之前重新读取最新文件内容；如果其他人已修改相同文件，应以新的主分支文件为依据继续编辑。

## 提交交接材料

代码变更完成并测试通过后，至少更新受影响的使用文档、技术说明、决策记录和维护台账。记录修改日期、涉及模块、验证命令及对应 GitHub Actions 结果。涉及正式发布时，还要记录 `CMakeLists.txt` 版本、Release 标签、相应提交和安装包名称。

把尚未实现的功能放在[产品功能评估](product-review.md)中，写明现有能力、建议的行为及验证方式；避免将计划写成已经具备的功能。
