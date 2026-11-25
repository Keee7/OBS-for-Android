# 参与贡献

欢迎通过 Issue 或 Pull Request 为 **OBS for Android** 贡献代码、文档或兼容设备测试报告（支持中文或英文交流）。

## 1. 提交问题反馈（Issues）

提交 Bug 时，请尽量提供以下信息以便快速定位：
1. **设备与系统**：设备型号、SoC 芯片架构、Android 系统版本（及厂商系统版本，如 HyperOS / ColorOS 等）。
2. **应用版本**：APK 版本号与构建 Git Commit SHA（可在「关于」或日志首行查看）。
3. **编解码信息**：实际工作的 MediaCodec 硬件组件名称（可从 OBS 日志获取）。
4. **场景配置**：使用的来源类型、画布/输出分辨率以及目标帧率。
5. **复现步骤**：清晰的复现路径，并说明问题发生在预览、本地录制还是直播推流阶段。
6. **脱敏日志**：附上 OBS 日志或 `adb logcat` 片段。**上传前请务必移除个人推流码（Stream Key）、账号 Token、私有局域网 IP 及个人路径信息**。

## 2. 提交代码（Pull Requests）

* **基线分支**：所有 PR 请基于 `main` 分支提出，保持每次改动聚焦且模块化。
* **开源合规**：所有新增代码必须遵循 **GPL-2.0-or-later** 许可证，完整保留上游版权与许可声明，不得引入与 GPLv2 不兼容的闭源或冲突协议依赖。
* **构建验证**：修改代码或打包脚本后，请参考 [BUILDING.md](https://github.com/Keee7/OBS-for-Android/blob/main/BUILDING.md) 构建 APK 并完成校验。
* **保持仓库干净**：请勿将编译产物（`.apk`、中间 `.so` 库、构建缓存目录）或私人签名密钥（`.jks` / `.keystore`）提交到 Git。

更多细节请参阅仓库根目录的 [CONTRIBUTING.md](https://github.com/Keee7/OBS-for-Android/blob/main/CONTRIBUTING.md)。
