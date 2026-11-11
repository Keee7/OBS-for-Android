# Media Playback, Hardware Decoding and Quality Mapping / 媒体源播放、硬件解码与画质控制指南

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This document describes media file playback via Android document URIs, FFmpeg MediaCodec hardware video decoding, and Advanced Output quality mapping.

### 1. Document URI Input & Custom AVIO Pipeline

Files selected via the system file picker are delivered as `content://` document URIs.

* **Android ContentResolver Bridge**: The implementation obtains a read-only, seekable File Descriptor (FD) through Android's ContentResolver and feeds it to FFmpeg via a custom AVIO context.
* **Capabilities**: Fully supports seek operations, seamless looping, and audio/video playback from local storage, preserving permissions across app restarts.

### 2. MediaCodec Hardware Video Decoding

In Media Source properties, enable "Use hardware decoding when available":

* **Decoder Selection**: For H.264, HEVC, VP8, VP9, and AV1, FFmpeg selects MediaCodec decoders that are flagged as hardware-accelerated and non-software components.
* **Graceful Fallback**: If hardware initialization fails or no suitable hardware component exists, playback falls back seamlessly to software decoders (including libaom for AV1).

### 3. Advanced Output Rate Control and Quality Mapping

In Advanced Output mode, MediaCodec encoders expose detailed rate control and quality options:

* **Rate Control**:
  * **CBR (Constant Bitrate)**: Recommended for live streaming.
  * **VBR (Variable Bitrate)**: Dynamic bitrate allocation according to scene complexity.
  * **CQ (Constant Quality)**: Recommended for local recording.
* **Hardware Quality Presets**:
  Abstract presets (Automatic, Low, Medium, High) are linearly mapped to the underlying component's supported complexity and quality ranges reported by the device driver.

---

<a name="简体中文"></a>
## 简体中文

本文档阐述 OBS for Android 中媒体文件的硬件解码回放机制，以及高级输出模式下的画质参数映射策略。

### 1. 媒体源与 Android 存储接入

在 Android 系统中，用户通过系统文件选择器选取的文件通常返回 `content://` 格式的文档 URI。

* **自定义 AVIO 管道**：本工程在底层接入 Android ContentResolver，获取底层的只读且可定位（Seekable）的文件描述符，直接传递给 FFmpeg 自定义 I/O 上下文；
* **特性**：完整支持大尺寸本地 MP4/MKV 媒体文件的流畅定位（Seek）、无缝循环播放以及应用重启后的权限持久化。

### 2. MediaCodec 硬件视频解码

在媒体源属性中，勾选“可用时使用硬件解码”：

* **解码器选择策略**：针对 H.264、HEVC、VP8、VP9 和 AV1 格式，FFmpeg 会自动探测并实例化标记为 `hardware-accelerated` 且非纯软件实现的 MediaCodec 解码器；
* **容错与回退**：若设备缺少对应硬件组件或解码器初始化失败，播放器会在初始化时尝试软件解码，AV1 使用 libaom。

### 3. 高级编码画质与复杂度调节

在“高级输出模式”下，MediaCodec 编码器提供细化的码控与质量映射：

* **Rate Control（码率控制）**：
  * **CBR（恒定码率）**：推荐用于网络推流，码率输出平稳；
  * **VBR（动态码率）**：根据画面动态度自动调整码率；
  * **CQ（恒定质量）**：适合本地录制，追求画质保真度。
* **Hardware Quality（硬件档位）**：
  提供 Automatic、Low、Medium、High 四个档位。系统会将这些抽象档位线性映射到底层 MediaCodec 驱动所公布的 `complexity` 与 `quality` 参数区间中。
