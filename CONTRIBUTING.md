# Contributing / 参与贡献

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

Contributions via Issues and Pull Requests are welcome in both English and Chinese.

### Reporting Issues

When reporting a bug, please provide the following details to help us triage efficiently:
1. **Device & OS**: Device model, SoC architecture, Android OS version (and OEM ROM version if applicable).
2. **App Version**: APK version string and build Git commit SHA.
3. **Codec Details**: Active MediaCodec hardware component name (available from logs).
4. **Scene Configuration**: Source types, base/output resolution, and target frame rate.
5. **Reproduction Steps**: Step-by-step reproduction instructions, distinguishing whether an issue occurs in preview, recording, or streaming.
6. **Sanitized Logs**: Attach relevant OBS logs or `adb logcat` output. **Always remove stream keys, personal credentials, private local network IPs, and private file paths before posting.**

### Code & Contribution Guidelines

* **Base Branch**: Please base pull requests on `main` and keep changes modular and focused.
* **Licensing**: All new code must comply with **GPL-2.0-or-later**. Preserve upstream copyright notices and licenses. Do not introduce dependencies incompatible with GPLv2+.
* **Verification Protocol**:
  * **Host Regression Tests**: For changes to shaders, math, layout policies, or state machines, provide corresponding offline unit tests (e.g., test scripts in `scripts/`).
  * **Device Validation**: For hardware codec, display projection, or USB changes, provide reproduction and verification evidence on real Android devices.
* **Clean Commits**: Do not commit generated build artifacts (APKs, `.so` binaries, intermediate build caches) or private keystores.

---

<a name="简体中文"></a>
## 简体中文

欢迎通过 Issue 或 Pull Request 为 OBS for Android 做出贡献。我们接受中文或英文交流。

### 问题反馈 (Reporting Issues)

提交 Bug 时，请尽量提供以下上下文以帮助快速定位问题：
1. **设备与系统**：设备型号、SoC 芯片架构、Android 系统版本（及厂商系统版本）；
2. **应用版本**：APK 版本号及构建 Git Commit SHA；
3. **编解码信息**：实际工作的 MediaCodec 硬件组件名称（可从日志获取）；
4. **场景配置**：来源类型、画布/输出分辨率与目标帧率；
5. **复现步骤**：清晰的复现路径，并标明异常发生在预览界面还是录像/推流输出中；
6. **脱敏日志**：提供 OBS 运行日志或 `adb logcat` 片段。**上传前请务必移除个人推流密钥（Stream Key）、敏感 Token、局域网 IP 及私有路径信息**。

### 贡献规范 (Code & PR Guidelines)

* **基线与分支**：所有 Pull Request 建议基于 `main` 分支提出，保持提交专注且原子化。
* **开源合规与许可**：新增代码必须遵循 **GPL-2.0-or-later** 许可证，保留原项目与第三方组件的版权声明。不得引入与 GPLv2 不兼容的第三方依赖。
* **分层验证原则**：
  * **主机测试**：涉及着色器、算法、布局策略或状态机逻辑修改时，请补充对应的离线单元测试（如 `scripts/` 下的 Python / C++ 测试工具）；
  * **真机测试**：涉及编解码器、屏幕采集或 USB 驱动层改动时，请说明在真实设备上的验证情况与实测表现。
* **干净提交**：请勿将编译产物（APK、中间依赖库 `.so`、构建缓存）或私人签名证书提交至 Git 仓库。
