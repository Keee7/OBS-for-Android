# Android 版本差异

本项目最低支持 **Android 10（API 29）**，编译基线为 API 36 / 目标 API 35。不同系统版本在权限、屏幕采集、HDR 与音频路径上存在如下差异：

| 系统版本 | 关键差异与新增能力 |
| --- | --- |
| **Android 10 – 12**<br/>*(API 29 – 32)* | • 支持全套核心功能（MediaCodec 硬编、MediaProjection 全屏采集与内录、Camera2、UVC、WebView、Hybrid MP4）。<br/>• 屏幕共享时仅支持**共享整个屏幕**，暂不支持单应用窗口隔离。<br/>• 硬件 **HEVC Main10 (HDR)** 不可用，若开 HDR 会自动走 `I010` + `x265` 软编。 |
| **Android 13**<br/>*(API 33)* | • 新增精细化 **通知权限（`POST_NOTIFICATIONS`）** 弹窗：必须允许通知才能正常在后台挂起 `ObsForegroundService`。<br/>• 开始支持系统级 **MediaCodec HEVC Main10 (10-bit HDR)** 硬件编码（需芯片驱动支持）。 |
| **Android 14 – 15**<br/>*(API 34 – 35)* | • 支持 **单应用屏幕共享**：授权弹窗可选择仅投屏某个游戏或 App，且接入 `onCapturedContentResize` 回调，切分屏/横竖屏时自动同步采集分辨率。<br/>• 系统强制要求前台服务精确声明 `specialUse \| mediaProjection \| camera \| microphone` 类型（已在清单中完整适配）；单次录屏授权仅对应单一持久 `VirtualDisplay` 实例，旋转屏幕不反复弹窗。 |
| **Android 16+**<br/>*(API 36+)* | • **AAudio MMAP 路径控制**：可在 AAudio 源属性中启用 `MMAP` 低延迟直通模式（低版本系统会自动忽略该项并走常规音频路径）。 |

> [!NOTE]
> 所有受 Android 10+ 分区存储（Scoped Storage）限制的版本，均统一通过 `ObsStorageHost`（MediaStore / SAF）写入 `Movies/OBS` 或用户授权目录，行为一致，无需担心系统版本差异导致录像路径失效。
