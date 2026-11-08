# OBS for Android

[English](README.md) | **简体中文**

在 Android 上使用 OBS 的场景、来源、录像和推流功能。本分支增加了 **MediaCodec H.264/HEVC 硬编、媒体源硬解、WebView 浏览器源、GPU 屏幕采集和共享目录录像**。

本项目基于 [cunminF/OBS_for_Android](https://github.com/cunminF/OBS_for_Android)，原移植基于 [OBS Studio 32.2.1](https://github.com/obsproject/obs-studio)。这是社区移植，并非 OBS Project 官方 Android 发行版。保留原作者、许可证声明和提交历史。

**当前测试版本：** `0.1.19-alpha-stability`，versionCode `20`。包名 `com.obsproject.studio`，**arm64-v8a，最低 Android 10 / API 29，目标 API 35**。Qt 界面主要面向横屏平板。开发测试包使用公开 debug 密钥，保留调试能力。

## 功能与验证状态

“已实现”表示源码和测试 APK 可构建，实机验证范围单独说明。

| 功能 | 实现及验证范围 |
| --- | --- |
| H.264 硬件编码 | 按名称创建硬件 MediaCodec 组件，Surface/GPU 输入。玄戒 O1 的 `c2.xring.avc.encoder` 已完成 1920×1080 / 60 fps 本地 RTMP 实测。 |
| HEVC / H.265 硬编 | Main 兼容 8-bit SDR、VPS/SPS/PPS 处理、能力检查和 Surface 输入。主机测试及构建通过，HEVC 实机录像仍待验证。 |
| 硬编质量控制 | 高级输出提供设备支持的 CBR/VBR/CQ 和自动、低、中、高档位，映射到组件 complexity/quality 范围。MediaCodec 不使用 x264 预设；画质和吞吐需按设备对比。 |
| GPU 屏幕采集 | MediaProjection → SurfaceTexture/OES → OBS 合成。新建源默认开启，失败退避重试，不自动回退 CPU，保留手动 CPU 选项。测试平板已确认 GPU 采集可用。 |
| 单应用采集与透明度 | Android 14+ 内容尺寸回调，兼容、直 Alpha、预乘 Alpha 模式。主机合成测试通过；VTube Studio → Android 投影 → OBS 的实际透明效果**尚未实机验证**。 |
| 浏览器源 | 系统 WebView 经 GPU 纹理接入，支持 URL、尺寸、帧率上限、CSS、刷新、显隐、基础指针交互和可选透明背景。透明模式改用 `ImageReader` 输出面（合成器走 GPU 合成保留 alpha）+ `AHardwareBuffer` 经 EGLImage 采样，零 CPU 拷贝；已在小米平板 25053RP5CC 实测。暂无 CEF 插件兼容和独立网页音轨。 |
| 音频输入 | AAudio 采集，输入设备来自 Android AudioManager（内置麦克风、有线耳机麦克风、USB 声卡/采集卡音频），可在源属性中按设备选择。UVC 采集卡的音频走系统 UAC 通道，前提是 ROM 暴露该 USB 输入设备。 |
| 媒体播放与硬解 | Android 文档 URI 输入，通过带补丁的 FFmpeg 选择命名 MediaCodec 硬解组件。在玄戒 O1 复现并修复 4K H.264 只显示一角，应用重启后复测正常；其他格式需单独验证。 |
| 共享目录录像 | MediaStore / SAF 写入 `Download/obs` 等可 seek 本地目录，修复路径容量查询失败被误报为“磁盘空间不足”。先使用混合 MP4/MOV；目录/提供者组合仍需实机测试。 |
| 小窗预览 | 统一 Qt/Android 窗口坐标，已检查进入小窗、移动、缩放和返回全屏的预览对齐。 |
| RTMP 推流 | 修复重连状态重置、CBR/VBR 动态码率及发送失败包释放。本地实机测试覆盖正常推流、主动断线与带宽限制。 |
| 1080p60 YUV 采集卡路径 | 独立 USB 收包、有界缓冲交接、标准 raw payload 重组、减少拷贝与 YUYV/NV12 低延迟模式。主机测试及构建检查通过，采集卡实际吞吐尚待验证。[专项说明](scripts/UVC_1080P60_TEST.md)。 |
| UVC 断流恢复 | 独占 USB 借用、旧描述符隔离、按有效帧超时恢复和限频 USB 软件重置，支持分类传输诊断；软件重置不保证等同物理断电。 |
| 相机与 USB 采集 | 保留原项目 Camera2、系统内录、Nikon PTP 和 UVC。原项目公开的设备结果属于上游验证，本分支未重复全部测试。 |

硬编初始化失败会明确报错，不静默替换为软件编码器。媒体源硬解可以在**初始化失败时**回退软件，与屏幕采集的 GPU 重试策略独立。

## 实机证据与性能范围

近期验证设备为 **Android 16、玄戒 O1 平板**，输出目标 1080p60：

- 本地 RTMP 接收 88.600 秒、5,317 帧，测得 60.000 fps，视频码率约 6.028 Mbps；接收文件完整解码无错，记录到渲染与编码各一次滞后。
- 接收器主动断开后约 1.267 秒重新连接成功，断线前后文件均完整解码。
- 本地限制为 2 Mbps 时，实际编码输出五秒窗口最低约 1.711 Mbps；解除后回升到约 5.836 Mbps，目标为 6 Mbps。
- 输入是已有的 4K H.264 媒体文件。输出 60 fps 不代表输入运动内容每秒有 60 个不同画面。

这些是 USB 转发后的本地 RTMP 结果，不能替代真实平台、Wi-Fi 丢包、RTMPS、长期温控或其他芯片测试。HEVC 与浏览器源性能仍待实机验证。详见 [实机记录](scripts/DEVICE_VALIDATION_WINDOW_STREAM.md)。

下图继承自原 Android 移植项目，用于展示界面，不代表本分支新增硬编测试的截图。

![原 Android 移植项目的屏幕采集预览](screenshot.png)

## 开始使用

1. 按 [构建说明](BUILDING.md) 生成测试 APK。APK 与依赖产物不提交进 Git；后续公开安装包放在 [Releases](https://github.com/Keee7/OBS-for-Android/releases)。
2. 使用 `adb install -r <test.apk>` 安装。更换签名或卸载旧包前先备份 OBS 配置。
3. 先设置 1920×1080、60 fps、SDR BT.709 和 Android MediaCodec H.264。高级输出先使用 CBR、自动质量，再按实际统计调整。
4. 添加屏幕源并同意系统共享授权。新建源默认使用 GPU，已有源明确保存的 CPU 选择会继续保留。重新打开源属性可查看接收帧计数。
5. 共享目录录像选择混合 MP4、关闭文件分割，输入 `/storage/emulated/0/Download/obs`，或通过系统目录选择器授权本地子目录。
6. 媒体源使用“浏览”选择视频并授权，勾选“可用时使用硬件解码”；初始化失败时查看日志中的回退原因。

透明合成测试：在屏幕源下面放颜色源或图片，系统共享时选择目标应用，尝试“**保留 Alpha（预乘输入）**”。该功能保留 Android 交付帧中已有的 Alpha；背景已被系统合成后无法恢复透明度。普通 H.264/HEVC 推流发送最终场景，不输出带 Alpha 的视频流。参见 [透明采集测试](scripts/TRANSPARENT_CAPTURE_TEST.md)。

## 如何确认硬件编码正在运行

录制 30–60 秒持续运动画面，收集同一次编码会话的日志：

```sh
adb logcat -v threadtime -d -s 'OBS-MediaCodec:*' > mediacodec-log.txt
python scripts/verify-mediacodec-log.py mediacodec-log.txt --codec h264
```

HEVC 使用 `--codec hevc`。需要同时看到：

- `event=started`、实际组件名及 `hardware=true software=false input=Surface`。
- `event=first-packet headers=valid keyframe=1`。
- `submitted`、`output`、`delivered`、`bytes` 持续增长。
- 停止时 `event=summary failed=0`，同一会话没有错误。

只选中了硬编名称或 CPU 占用低，都不足以证明硬编正常产出。还要用 FFmpeg/ffprobe 检查文件，观察 OBS 渲染延迟和编码跳帧。[硬编检查文档](scripts/HEVC_HARDWARE_TEST.md) 有完整步骤。

## 当前限制

- 硬件能力、吞吐和质量取决于系统组件；未实现 HDR/Main10，不保证所有设备持续 1080p60。
- GPU 屏幕采集失败后重试，不自动切换 CPU；共享授权被收回后必须重新同意。
- 媒体源硬解输出仍为 CPU 可读帧，不是端到端 GPU 零拷贝。
- 共享文档录像需要可 seek 的本地提供者；该路径暂不支持分割录像、回放缓存、自定义 FFmpeg 输出和自动 remux。真实空间不足仍会停止录像。
- 浏览器源暂无独立音轨、本地 HTML 选择器和完整键盘/滚轮/多点触控；透明背景是可选开关，能否穿过设备虚拟显示合成需实机验证，明文 HTTP 受 Android 网络策略限制。
- 未实现 NDI/Spout2 输入，VTube Studio 原生透明采集仍待实机验证。
- 脚本和 Twitch/YouTube/Restream 内置服务集成仍禁用，需手动配置受支持的服务器与推流密钥。

## 仓库与文档

```text
obs-studio/          OBS 源码、Android 平台/前端和插件
  plugins/android-mediacodec/   H.264/HEVC 硬件编码器
  plugins/android-screen/       GPU/CPU 屏幕采集及 Alpha 处理
  plugins/android-browser/      WebView 浏览器源
android-shell/       早期测试壳和公开开发调试密钥
deps-android/        依赖版本、下载及 WSL/Linux 交叉构建
scripts/             Windows 构建辅助、FFmpeg 补丁与测试文档
```

[构建](BUILDING.md) · [参与贡献](CONTRIBUTING.md) · [最新测试包说明](scripts/DEVICE_TEST_QUICKSTART.md) · [媒体与质量](scripts/MEDIA_QUALITY_TEST.md) · [存储](scripts/ANDROID_STORAGE_TEST.md) · [屏幕采集](scripts/ANDROID_PROJECTION_TEST.md) · [1080p60 验收](scripts/android-1080p60-acceptance.md)

[工程记录](IMPLEMENTATION_ANDROID.md) 和部分测试文档保留了历史版本的结论，当前默认行为以上文和最新测试包说明为准。[反馈问题](https://github.com/Keee7/OBS-for-Android/issues) 时提供设备/应用/Android 版本、组件名称、复现步骤和已脱敏日志，删除推流密钥与私人路径。

## 许可证与致谢

本移植沿用 OBS Studio 的 **GPL-2.0-or-later**。参见 [LICENSE](LICENSE)、[OBS COPYING](obs-studio/COPYING)、[OBS AUTHORS](obs-studio/AUTHORS) 及 [NOTICE.md](NOTICE.md)。依赖保留各自许可证。仓库内 debug keystore 是公开开发材料，不是私有发行签名密钥。

感谢 [cunminF](https://github.com/cunminF/OBS_for_Android) 的原始移植、[OBS Project](https://github.com/obsproject/obs-studio) 及各依赖作者。OBS 名称和图标的权利与署名归其各自所有者。
