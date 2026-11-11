# Software Codecs, Image Documents, and IME Diagnostics / 软件编码、图像 URI 与输入法技术文档

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This document describes the software encoding pipelines (x265 and libaom), Android document URI handling for image sources, and Input Method Editor (IME) event handling.

### 1. Software HEVC and AV1 Encoding

* **Integration**: Integrates `ffmpeg_x265` (Software HEVC / x265 3.6) and `ffmpeg_aom_av1` (AOM AV1 3.10.0) alongside FFmpeg 8.0.3.
* **Presets & Threading**:
  * Simple Mode provides independent encoding presets for streaming and recording.
  * Advanced Mode exposes CRF/CQP rate control, CPU speed, and thread limits.
  * x265 defaults to `ultrafast` 8-bit SDR; AOM AV1 defaults to `realtime` mode with row multithreading enabled.
* **ARM64 Assembly Optimization**: Retains ARM64 NEON assembly routines in x265 with patches for Android NDK r30 toolchain link flags.

### 2. Image Source Document URIs (`content://`)

* **SAF Document Access**: Resolves `content://` and `file://` document URIs using Android's storage bridge, obtaining a seekable File Descriptor (FD) without pre-copying files to application cache.
* **Format Probing**: Probes PNG, JPEG, and GIF formats directly from custom AVIO data streams, supporting extensionless URIs.

### 3. IME Event Handling

* **Diagnostics**: Optional diagnostics via `OBS_ANDROID_IME_DIAGNOSTICS=1` or the `--android-ime-diagnostics` flag track focus transitions, selection boundaries, and coordinate mappings without recording user input text.
* **Backing Fixes**: Addresses duplicate show requests and floating cursor handle stranded popups via Qt Android runtime patches.

---

<a name="简体中文"></a>
## 简体中文

本文档阐述 H.265/AV1 软件编码管线、静态图像 Android 文档 URI 接入以及输入法（IME）事件处理机制。

### 1. H.265 与 AV1 软件编码

* **架构集成**：在 FFmpeg 8.0.3 基础上接入 `ffmpeg_x265`（x265 3.6）与 `ffmpeg_aom_av1`（AOM AV1 3.10.0），构建 ARM64 交叉编译支持；
* **预设与多线程控制**：
  * 简单输出模式下可分别为推流与录像配置独立预设；
  * 高级输出模式下开放 CRF/CQP 码控、速度档位与工作线程上限设置；
  * x265 默认使用 `ultrafast` 8-bit SDR 档位，AOM AV1 默认采用 `realtime` 模式并启用行并行处理；
* **汇编优化**：保留 x265 ARM64 NEON 汇编优化，修正了 NDK r30 下 pkg-config 私有链接项。

### 2. 图像源的 Document URI (`content://`) 接入

* **SAF 文档无缝读取**：通过 Android 存储桥接解析系统文件选择器返回的 `content://` 和 `file://` 文档 URI，通过可定位的文件描述符交由 FFmpeg 自定义 AVIO 处理，无需将大图预先完整复制到应用私有缓存；
* **格式自动探测**：依据数据流头部特征自动探测 PNG、JPEG 与 GIF 格式，兼容不带扩展名的标准 Android 文档 URI。

### 3. 输入法（IME）事件与光标诊断

* **诊断系统**：提供 `OBS_ANDROID_IME_DIAGNOSTICS=1` 环境变量与 `--android-ime-diagnostics` 启动参数，用于安全跟踪编辑框焦点迁移与坐标映射（不记录任何敏感文字输入内容）；
* **运行时修复**：结合 Qt Android 运行时补丁，消除软键盘退格时反复触发的系统弹起请求，并完善手柄失焦清理。
