# HEVC Hardware Encoding and GPU Capture Guide / HEVC 硬件编码与 GPU 屏幕采集测试指南

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This guide describes how to configure and verify MediaCodec HEVC (H.265) hardware encoding and GPU screen capture pipelines on Android devices.

### 1. Enabling MediaCodec HEVC Encoding

1. Navigate to **Settings → Output**:
   * Switch "Output Mode" to **Advanced**.
   * Under the "Recording" tab, set Encoder to **Android MediaCodec HEVC (H.265)**.
   * Container format is recommended as **Hybrid MP4**.
2. **Color & Format Specifications**:
   * Standard 8-bit SDR BT.709 color format.
   * Surface direct input with non-B-frame configuration for low latency.
3. **Rate Control**:
   * For local recording tests, CBR or CQ (Constant Quality) is recommended.
   * Hardware Quality preset can be set to Automatic or Medium/High.

### 2. GPU Screen Capture Pipeline

The screen capture source properties provide the "GPU Screen Capture" toggle:
* **GPU Mode (Default & Recommended)**: MediaProjection frames route directly to an OpenGL ES OES external texture within the OBS composition pipeline, bypassing CPU readbacks for lower power and memory usage.
* **Fallback Behavior**: If the hardware display surface fails to initialize, the system performs an exponential retry backoff while keeping a manual CPU fallback option available.

### 3. Log Verification via ADB

Start a recording session, then capture encoder logs:

```sh
adb logcat -v threadtime -d -s 'OBS-MediaCodec:*' > hevc-session.log
python scripts/verify-mediacodec-log.py hevc-session.log --codec hevc
```

Expected log indicators:
* `event=started codec=hevc hardware=true software=false input=Surface`: Confirms hardware component configuration and launch.
* `event=first-packet headers=valid keyframe=1`: Confirms valid parameter sets and initial keyframe.
* `event=progress`: `submitted`, `output`, and `delivered` counters increment monotonically without unhandled session errors.

---

<a name="简体中文"></a>
## 简体中文

本指南说明如何在 Android 设备上配置并验证 MediaCodec HEVC (H.265) 硬件编码及 GPU 屏幕采集管线。

### 1. 启用 MediaCodec HEVC 编码

1. 进入 **设置 → 输出**：
   * 将“输出模式”切换为“高级”；
   * 在“录像”选项卡中，编码器选择 **Android MediaCodec HEVC (H.265)**；
   * 容器格式推荐选择 **Hybrid MP4**。
2. **色彩与规格**：
   * 编码器管线支持标准的 8-bit SDR BT.709 色彩格式；
   * 默认采用无 B 帧的 Surface 直连输入以获得低延迟。
3. **码率控制**：
   * 录制测试推荐使用 CBR 或 CQ（恒定质量）模式；
   * 硬件质量档位可设为“自动”或根据需要选择“中 / 高”。

### 2. GPU 屏幕采集管线

屏幕采集源属性中提供了“GPU 屏幕采集”选项：
* **GPU 模式（推荐默认）**：MediaProjection 输出的视频帧直接挂载为 OpenGL ES OES 外部纹理，进入 OBS 合成管线，消除 CPU 逐帧回读，显著降低整体功耗与内存开销。
* **兼容回退模式**：若设备底层 Surface 初始化异常，系统将执行指数退避重试，同时保留手动切换至 CPU 采集路径的兼容选项。

### 3. 编码日志与分析方法

启动录像后，通过以下命令抓取编码器运行状态：

```sh
adb logcat -v threadtime -d -s 'OBS-MediaCodec:*' > hevc-session.log
python scripts/verify-mediacodec-log.py hevc-session.log --codec hevc
```

正常工作指标：
* `event=started codec=hevc hardware=true software=false input=Surface`：已按硬件规格完成组件配置并启动；
* `event=first-packet headers=valid keyframe=1`：首个关键帧及参数集成功交付；
* `event=progress`：`submitted`（输入帧）、`output`（编码帧）及 `delivered`（交付包）计数保持健康增长，且未触发会话级异常。
