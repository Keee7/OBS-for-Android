# OBS for Android 使用文档

欢迎使用 **OBS for Android**！这是基于 OBS Studio v33 移植到 Android 手机与平板的直播推流与本地录制工具，支持 MediaCodec 硬件加速、多路音视频采集以及横屏自适应工作区。

---

## 文档目录

### 快速入门
* [下载安装](quick-start/download-and-install.md) — APK 安装、ADB 安装、应用内更新检查与源码编译入口
* [系统与设备要求](quick-start/system-requirements.md) — 支持架构/版本、权限清单、与桌面版核心差异
* [界面导览与触控操作](quick-start/interface-overview.md) — 平板/手机双形态布局、`☰` 菜单与触控技巧
* [第一次直播](quick-start/first-stream.md) — 5 分钟快速跑通视频、编码、来源与推流设置

### 场景与来源
* [场景管理](scenes-and-sources/scene-management.md) — 来源层叠、工作室模式、转场动画与滤镜
* [摄像头与屏幕采集](scenes-and-sources/camera-and-screen.md) — MediaProjection 单应用录制、Camera2（一键前后置）、Nikon Z 系列
* [USB/UVC 采集卡](scenes-and-sources/usb-uvc-capture.md) — USB 授权、MJPEG/YUV 档位、低延迟与断流自愈
* [安卓浏览器与弹幕叠加](scenes-and-sources/browser-overlay.md) — WebView 透明弹幕、自定义 CSS、音频控制与单指交互
* [网络媒体源](scenes-and-sources/network-media-source.md) — 本地文件播放与 SRT / RIST / RTSP / HLS 等拉流

### 直播与录制
* [RTMP / SRT 推流与协议说明](streaming-and-recording/rtmp-and-srt.md) — 自定义 RTMP/RTMPS 推流与拉流协议边界说明
* [视频编码器](streaming-and-recording/video-encoders.md) — MediaCodec H.264/HEVC/AV1/VP9 与软件编码器选型对照
* [音频设置](streaming-and-recording/audio-settings.md) — AAudio 输入预设、独占/低延迟与 RNNoise 降噪滤镜
* [本地录制](streaming-and-recording/local-recording.md) — 混合 MP4/MOV、SAF 共享存储目录直录与成片查看
* [后台保活与防杀指南](streaming-and-recording/background-keepalive.md) — 前台服务机制与国产系统省电/锁卡防杀配置

### 高级功能
* [HDR 与色彩空间](advanced/hdr-and-colorspace.md) — Rec. 2100 PQ/HLG、P010 硬编与 I010 软编、SDR 白电平
* [硬件编码](advanced/hardware-encoding.md) — MediaCodec 零拷贝 Surface 架构、CBR/VBR/CQ 与 ADB 验证
* [性能与功耗优化](advanced/performance-and-power.md) — 局部脏区渲染、分辨率帧率取舍与散热供电建议
* [NDI® 局域网收发与场景配方](advanced/ndi-and-recipes.md) — 导入官方 `libndi.so`、无线收发与 3 套开箱场景配方

### 兼容性
* [SoC 与编码器支持](compatibility/soc-and-encoders.md) — 各芯片对 H.264 / HEVC Main10 / AV1 / Full Range 的支持规律
* [采集卡兼容性](compatibility/capture-card-compatibility.md) — USB 3.0 线材要求、不同采集卡推荐格式与 UAC 伴音
* [Android 版本差异](compatibility/android-versions.md) — Android 10–12 / 13+ / 14+ / API 36+ 特性与权限对照

### 故障排查
* [采集黑屏](troubleshooting/capture-black-screen.md) — 屏幕采集、Camera2、UVC 与浏览器源黑屏排查
* [直播卡顿](troubleshooting/stream-stuttering.md) — 软编误用、网络波动与 USB 带宽不足排查
* [编码器报错](troubleshooting/encoder-errors.md) — 回退提示、5 秒超时与存储阈值报错速查
* [日志收集与问题反馈](troubleshooting/logs-and-feedback.md) — 应用内日志、`adb logcat` 与插件管理器排查

### 项目相关
* [更新日志](project/changelog.md) — v0.2.5 / v0.2.6 / v0.2.7 / v0.2.8 / v0.2.9 / v0.2.10 版本演进摘要
* [参与贡献](project/contributing.md) — Issue 反馈模板、PR 规范与脱敏提醒
* [开源许可证与声明](project/license.md) — GPL-2.0-or-later、第三方依赖与商标声明
