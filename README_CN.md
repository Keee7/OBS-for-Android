<div align="center">

  <p align="center">
    <a href="https://obsproject.com" target="_blank" rel="noopener noreferrer">
      <img src="assets/obs-logo.png" width="58" height="58" alt="OBS Studio Logo" style="vertical-align: middle;" />
    </a>
    &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
    <a href="https://www.android.com" target="_blank" rel="noopener noreferrer">
      <img src="assets/android-logo.svg" width="58" height="58" alt="Android Logo" style="vertical-align: middle;" />
    </a>
  </p>

  <h1>OBS for Android</h1>

  <p>
    <b>Android 平台高性能直播推流与本地录制工具</b><br />
    <sub>基于 OBS Studio 33 · MediaCodec 硬件加速 · OpenGL ES · 自适应横屏交互</sub>
  </p>

  <p>
    <a href="https://github.com/Keee7/OBS-for-Android/releases"><img src="https://img.shields.io/github/v/release/Keee7/OBS-for-Android?style=flat-square&color=2563EB&label=Release" alt="Release"></a>
    <a href="https://github.com/Keee7/OBS-for-Android/actions/workflows/build-apk.yml"><img src="https://img.shields.io/github/actions/workflow/status/Keee7/OBS-for-Android/build-apk.yml?branch=main&style=flat-square&label=Build" alt="Build Status"></a>
    <img src="https://img.shields.io/badge/Platform-Android%2010%2B%20(API%2029%2B)-3DDC84?style=flat-square&logo=android&logoColor=white" alt="Platform">
    <img src="https://img.shields.io/badge/Arch-arm64--v8a-4F46E5?style=flat-square" alt="Architecture">
    <img src="https://img.shields.io/badge/OBS%20Studio-v33.x-302E31?style=flat-square&logo=obsstudio&logoColor=white" alt="OBS Baseline">
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPL--2.0--or--later-10B981?style=flat-square" alt="License"></a>
  </p>

  <p>
    <a href="README.md"><b>English</b></a> &nbsp;•&nbsp;
    <a href="README_CN.md"><b>简体中文</b></a> &nbsp;•&nbsp;
    <a href="https://github.com/Keee7/OBS-for-Android/releases"><b>下载安装包</b></a> &nbsp;•&nbsp;
    <a href="BUILDING.md"><b>构建指南</b></a> &nbsp;•&nbsp;
    <a href="IMPLEMENTATION_ANDROID.md"><b>架构实现</b></a> &nbsp;•&nbsp;
    <a href="CONTRIBUTING.md"><b>参与贡献</b></a>
  </p>

  <br />

  <p align="center">
    <img src="screenshot.png" alt="OBS for Android 预览截图" width="90%" style="max-width: 840px; border-radius: 10px; box-shadow: 0 8px 30px rgba(0,0,0,0.2);" />
  </p>

</div>

---

[OBS Studio](https://github.com/obsproject/obs-studio)（`v33` 基线）的 Android 移植版本，在 [cunminF/OBS_for_Android](https://github.com/cunminF/OBS_for_Android) 基础上整合了硬件编解码、GPU 采集、外接设备输入与自适应横屏界面，支持场景合成、本地录制与直播推流。

## 核心特性

| 模块 | 能力摘要 |
| --- | --- |
| **MediaCodec 硬件编码** | 支持 H.264、HEVC（Android 13+ HDR 下支持 Main10）、AV1（推流与 Hybrid MP4）及 VP9（Hybrid MP4），采用零拷贝 Surface 输入，支持 `CBR` / `VBR` / `CQ`。 |
| **软件编码器** | 内置 `x264`、`x265`（`3.6`，8/10-bit）与 `libaom`（`3.10.0`）AV1。 |
| **10-bit HDR 管线** | 支持 Rec. 2100 PQ/HLG 合成、`P010` 硬件编解码与 `I010` 软编路径，并在 Hybrid MP4 中保留 HDR 色彩元数据。 |
| **屏幕与单应用采集** | MediaProjection GPU 零拷贝采集（`SurfaceTexture`/OES，可选 CPU 回退），支持 Android 14+ 单应用内容尺寸自适应及直通/预乘 Alpha 透明合成。 |
| **WebView 浏览器源** | 系统 WebView GPU 纹理采集，支持网页/本地 HTML、自定义 CSS、触摸交互、零拷贝透明背景及“通过 OBS 控制音频”。 |
| **USB 与相机采集** | 支持 USB 3.0 UVC 采集卡/摄像头（`YUYV`、`NV12`、`RGB24`、`BGR24`、`MJPEG`，自动匹配 UAC 音频并具备断流恢复）、内置 Camera2 NDK 与 Nikon Z 系列 PTP 实时取景。 |
| **媒体与网络拉流** | FFmpeg MediaCodec 硬件解码（H.264、HEVC、VP8、VP9、AV1，失败自动回退软解）；支持文档 URI 与 `HTTP(S)`、`HLS`、`DASH`、`RTSP(S)`、`RTMP(S)`、`SRT`、`RIST`、`UDP`/`RTP`/`TCP` 拉流。 |
| **音频路由** | 原生 AAudio 引擎，支持内置麦克风、有线耳机与 USB 声卡/采集卡音频输入。 |
| **共享存储录制** | 通过 MediaStore / SAF 文件描述符桥接，直接将 Hybrid MP4/MOV 写入 `Download/obs` 等公共目录。 |
| **自适应横屏界面** | 基于 Qt `6.12` 的响应式布局（逻辑分辨率 `< 952×552` 自动切换手机紧凑模式，平板展开宽松布局），适配分屏、小窗与输入法交互。 |

## 运行环境

* **目标架构与系统**：`arm64-v8a`，Android 10+（API 29+），目标 API 35（编译 API 36）。
* **运行依赖**：浏览器源依赖 Android 系统 WebView；硬件编解码档位与 10-bit HDR 渲染目标取决于设备硬件支持。

## 快速上手

1. **安装应用**：从 [Releases](https://github.com/Keee7/OBS-for-Android/releases) 下载 APK 并执行 `adb install -r path/to/obs-android.apk`，或参考 [BUILDING.md](BUILDING.md) 自行编译。
2. **推荐初始配置**：
   * **视频**：`1920×1080` @ `60 FPS`，`SDR BT.709`（启用 10-bit HDR 时，在 **设置 → 高级 → 视频** 选择 `Rec. 2100 (PQ)` 或 `Rec. 2100 (HLG)`，配合 `P010` + `MediaCodec HEVC Main10` 或 `I010` + `x265` / `libaom`）。
   * **输出**：推流选择 `Android MediaCodec H.264`（`CBR`）；录像建议 `Hybrid MP4`，保存路径设为 `/storage/emulated/0/Download/obs`。
3. **添加来源**：
   * 按需添加 **屏幕采集（MediaProjection）**（授予录屏权限，取消后可在源属性中点击“请求屏幕共享授权”重新拉起）、**UVC 摄像头（Android USB）**、**内置摄像头（Camera2）**、**Nikon Webcam（Z 系列）** 或 **安卓浏览器**。
   * 拉取网络流时，添加 **媒体源** 并取消勾选 **本地文件**，填入 `HTTP(S)`、`HLS`、`DASH`、`RTSP`、`RTMP`、`SRT` 或 `RIST` 地址（必要时在 **FFmpeg 选项** 填写如 `rtsp_transport=tcp`）。
4. **验证硬件编码** *（可选）*：
   ```sh
   adb logcat -v threadtime -d -s 'OBS-MediaCodec:*'
   ```
   确认日志包含 `event=started hardware=true software=false input=Surface`、`event=first-packet headers=valid keyframe=1`，且 `event=progress` 帧数与字节数持续增长。

## 已知限制

* **HDR / 10-bit**：依赖 GPU 对 16-bit 归一化纹理与浮点渲染目标的支持；界面预览与 Android 屏幕采集仍按配置的 SDR 白电平以 SDR 混入场景，P010 USB 采集需采集卡硬件支持。
* **浏览器源**：暂不支持多指触控手势；跨域网页音频路由到 OBS 混音器需服务端支持 CORS。
* **推流服务**：Twitch / YouTube OAuth 快捷登录已禁用，请使用“自定义”服务手动填写 RTMP 服务器与推流码。

## 文档与开源许可

* [BUILDING.md](BUILDING.md) — GitHub Actions、Linux/WSL 与 Windows 构建说明
* [IMPLEMENTATION_ANDROID.md](IMPLEMENTATION_ANDROID.md) — EGL/MediaCodec、MediaProjection、UVC、SAF 存储与 Qt 界面架构细节
* [CONTRIBUTING.md](CONTRIBUTING.md) — 问题反馈与贡献规范
* [NOTICE.md](NOTICE.md) — 第三方依赖许可证与商标声明

本项目遵循 **GPL-2.0-or-later** 许可证（[LICENSE](LICENSE)），为社区维护的独立移植版本，与 OBS Project 无关。
