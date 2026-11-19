# 项目文档目录

本目录提供项目使用、开发、交接、测试和正式发布的说明。以当前 `main` 分支代码为准；正式发布包对应的源提交应单独从 GitHub Release 核对。

## 建议阅读顺序

| 读者或任务 | 建议先阅读 | 说明 |
|---|---|---|
| 首次使用软件 | [README](../README.md) | 安装、导入文件、编码、裁剪和常用操作 |
| 新开发者接手项目 | [接管与维护手册](handover.md) → [开发者接管指南](developer-handoff.md) | 准备环境、熟悉代码、首次构建和验证 |
| 了解实现方式 | [程序结构](architecture.md) → [项目决策记录](decision-log.md) | 模块边界、安全要求、关键设计选择 |
| 修改视频转换 | [编码参数说明](encoding.md) | 编码器、质量、容器、音频和裁剪参数 |
| 修改任务保存或导入 | [任务清单格式](queue-format.md) | `.ffq` 数据格式、恢复和兼容性 |
| 修改 SSH 功能 | [SSH 远程编码](ssh-remote-processing.md) | 连接、认证、传输、安全和清理 |
| 测试、生成安装包或发布 | [构建、测试与发布手册](ci-release.md) → [Windows 安装与第三方组件](windows-distribution.md) | 命令、CI、安装包及第三方组件许可 |
| 了解工作记录 | [维护台账](maintenance-ledger.md) → [开发状态记录](development-checkpoint.md) | 已完成工作、验证记录及主分支现状 |
| 规划产品后续开发 | [产品功能评估](product-review.md) | 现有能力及可考虑的后续功能 |

## 文档维护

- 代码实现、测试和文档使用相同的术语与文件路径。
- 修改公共行为时，同步更新 README、相关专业说明和维护台账。
- 修改队列持久化或裁剪逻辑时，同步更新 `queue-format.md` 与 `architecture.md`。
- 修改构建、依赖或发布过程时，同步更新 `ci-release.md` 和适用的平台分发说明。
- 开发状态以实际主分支提交和对应 CI 结果为准；已经发布的安装包以 GitHub Release 的源提交为准。
