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
    <b>专为移动端重构的高性能 Android 版 OBS Studio</b><br />
    <sub>基于 OBS Studio 33 · MediaCodec 硬件加速 · GPU 零拷贝采集 · 横屏自适应布局</sub>
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
    <a href="CONTRIBUTING.md"><b>参与贡献</b></a>
  </p>

  <br />

  <p align="center">
    <img src="screenshot.png" alt="OBS for Android Preview" width="90%" style="max-width: 840px; border-radius: 10px; box-shadow: 0 8px 30px rgba(0,0,0,0.2);" />
  </p>

  <p align="center">
    <code>⚡ MediaCodec 硬编解码</code> &nbsp;•&nbsp;
    <code>📱 响应式横屏布局</code> &nbsp;•&nbsp;
    <code>🎥 GPU 屏幕采集</code> &nbsp;•&nbsp;
    <code>🌐 WebView 浏览器源</code> &nbsp;•&nbsp;
    <code>🔌 UVC 1080p60 采集</code>
  </p>

</div>

---

OBS Studio 的 Android 平台移植版本，支持场景编辑、多来源输入、本地录像与 RTMP 实时推流。

本分支基于 [cunminF/OBS_for_Android](https://github.com/cunminF/OBS_for_Android)，并已同步至 [OBS Studio 33](https://github.com/obsproject/obs-studio) 上游基线。在此基础上，新增了 **MediaCodec 硬件编解码、HEVC/AV1 软件编码、GPU 屏幕采集、WebView 浏览器源、UVC 采集卡输入优化、Android 共享存储录像以及多设备横屏自适应响应式布局**。

> **开源说明**：本项目为社区维护的开源移植版本，遵循 **GPL-2.0-or-later** 许可证，并非 OBS Project 官方 Android 发行版。完整保留原项目提交历史、作者署名与开源许可证。

---

## 核心特性

| 功能模块 | 说明与实现细节 |
| --- | --- |
| **MediaCodec 硬件编码** | 支持 H.264、HEVC (H.265)、AV1 与 VP9 硬件编码。采用 Surface 输入，避免整帧 CPU 回读。简单模式只列出支持 CBR 的硬编，高级模式提供设备支持的 CBR、VBR、CQ 与质量设置。AV1 可用于兼容的推流服务与混合 MP4 录像，VP9 可用于混合 MP4 录像。 |
| **H.265 / AV1 软件编码** | 集成 x265 (3.6) 与 libaom (3.10.0)。支持推流与录像独立配置编码预设，提供多线程与速度优化选项。 |
| **GPU 屏幕采集** | 基于 Android MediaProjection API，通过 SurfaceTexture / OES 纹理直入 OBS 渲染合成管线，失败后退避重试，保留手动选择 CPU 采集的选项。 |
| **单应用捕获与 Alpha 透明通道** | 支持 Android 14+ 单应用内容尺寸自适应；提供直通 Alpha 与预乘 Alpha 合成模式，透明合成取决于 Android 交付帧是否保留 Alpha。 |
| **WebView 浏览器源** | 基于系统 WebView 渲染并接入 GPU 纹理，支持自定义 URL、分辨率、帧率上限、CSS 注入与基本触摸交互；透明背景模式通过 ImageReader + EGLImage 实现零拷贝合成。 |
| **音频输入与路由** | 基于 AAudio 音频引擎，支持内置麦克风、有线耳机及 USB 声卡 / 采集卡 UAC 音频输入的动态枚举与选择。 |
| **媒体播放与硬件解码** | 支持 Android 文档 URI 输入；FFmpeg 优先选用平台硬件 MediaCodec 解码器（H.264、HEVC、VP8、VP9、AV1），初始化失败时回退到软解，AV1 使用 libaom。 |
| **Android 共享存储录像** | 深度适配 MediaStore 与 Storage Access Framework (SAF)，可直接将混合 MP4/MOV 录制至 `Download/obs` 等公共共享目录，解决分区存储容量误判问题。 |
| **UVC 采集卡高速通道** | 针对 USB 3.0 / SuperSpeed 1080p60 采集卡重构数据泵：独立收包线程、有界缓冲交接、Raw YUV (YUYV/NV12) 直接重组与断流自动看门狗恢复。 |
| **响应式布局与横屏交互** | 内置基于动态断点（`952×552` 逻辑像素）的自适应布局引擎。专为横屏场景设计，在手机横屏紧凑模式（32px 行高、折叠空闲工具栏）与平板宽松触控模式（48px 行高）之间无缝切换，并对悬浮小窗与多任务分屏进行边界几何对齐。 |
| **Qt 界面与输入法适配** | 统一窗口坐标几何系统；优化软键盘事件处理，解决连续退格请求风暴、光标手柄偏移及对话框关闭后残影问题。 |

---

## 运行环境与设备形态

* **架构**：`arm64-v8a`
* **系统版本**：Android 10+（API 29+），目标 API 35
* **设备形态适配**：
  * **手机设备**：横屏握持下自动启用**横屏紧凑布局**，将列表行高压缩至 32px，并动态折叠空闲工具栏，为小屏幕换取最大的视频画布预览空间；
  * **平板与大屏设备**：自动展开为多栏**宽松触控布局**（48px 行高），提供直观高效的大屏操作体验；
  * **多任务分屏与小窗**：界面根据窗口实际尺寸在紧凑与宽松模式间平滑过渡，具备 48px 滞回保护以消除临界闪烁。
* **依赖组件**：浏览器源需要系统 WebView。硬件编解码选项取决于设备能力。

---

## 参考测试基准

在参考测试设备（Android 16 移动设备，1080p60 输出目标）上的实测数据：

* **本地 RTMP 推流**：1920×1080、60 fps，目标 CBR 6 Mbps。接收端在 88.600 秒内收到 5,317 帧，实测约 6.028 Mbps。OBS 记录了 1 帧渲染滞后和 1 帧编码滞后。
* **断线重连恢复**：模拟接收端断线后约 1.267 秒重连。
* **UVC 采集**：NV12 SuperSpeed 模式恢复后的稳定采样约为 1080p60、186.8 MB/s。

> **说明**：以上为本地测试结果。测试环境和范围详见 [RTMP 实测记录](scripts/DEVICE_VALIDATION_WINDOW_STREAM.md) 与 [IME/UVC 测试记录](scripts/IME_UVC_TEST.md)。

---

## 快速上手

1. **获取安装包**：从 [Releases](https://github.com/Keee7/OBS-for-Android/releases) 下载最新的测试 APK，或参考 [构建说明](BUILDING.md) 自行编译。
2. **安装**：
   ```sh
   adb install -r path/to/obs-android.apk
   ```
3. **推荐初始设置**：
   * **视频**：基础画布与输出分辨率设为 `1920×1080`，FPS 设为 `60`，色彩空间选择 `SDR BT.709`。
   * **输出（推流）**：编码器选用 `Android MediaCodec H.264`，码率控制选择 `CBR`。
   * **录像存储**：录像格式建议选择 `Hybrid MP4`，路径设为 `/storage/emulated/0/Download/obs`。
4. **添加采集源**：
   * 点击来源面板添加“屏幕采集”，并在系统弹窗中授予录屏权限。
   * 若需采集 USB 采集卡画面，添加“UVC 摄像头”并选择对应的分辨率与 YUV 格式。

---

## 硬件编码状态验证

可通过 ADB 日志确认 MediaCodec 硬件加速正在正常工作：

```sh
adb logcat -v threadtime -d -s 'OBS-MediaCodec:*' > mediacodec-log.txt
python scripts/verify-mediacodec-log.py mediacodec-log.txt --codec h264
```

正常工作时的关键日志特征：
* `event=started hardware=true software=false input=Surface`（硬件组件已加载且输入为 Surface）
* `event=first-packet headers=valid keyframe=1`（已输出首个关键帧）
* `event=progress` 中输出帧计数和字节数持续递增。

脚本支持 H.264 和 HEVC。AV1 与 VP9 请检查会话日志，并用 `ffprobe` 核对录像编码格式。详见 [硬件编码测试指南](scripts/HEVC_HARDWARE_TEST.md)。

---

## 已知局限与计划

* **HDR / 10-bit**：当前主要支持 8-bit SDR BT.709 硬件管线，暂未开启 HDR/Main10。
* **浏览器源高级能力**：暂未集成独立网页音频通道及多指触控手势。
* **推流服务集成**：Twitch / YouTube 账户快捷登录暂未启用，请使用“自定义”服务输入 RTMP 服务器地址与推流码。

---

## 许可证与致谢

本项目遵循 **GPL-2.0-or-later** 许可证。详情请参阅 [LICENSE](LICENSE) 与 [NOTICE.md](NOTICE.md)。

* 感谢 [cunminF/OBS_for_Android](https://github.com/cunminF/OBS_for_Android) 奠定的初始 Android 移植基础。
* 感谢 [OBS Studio](https://github.com/obsproject/obs-studio) 官方团队及各上游开源组件作者。
