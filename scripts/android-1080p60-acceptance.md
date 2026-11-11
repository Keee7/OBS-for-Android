# 1080p60 Streaming & Recording Acceptance Criteria / 1080p60 流畅度与性能验收规范

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This document defines the acceptance criteria and verification methodology for sustained 1080p60 streaming and recording on Android devices.

### Acceptance Criteria

1. **Video Stream Parameters**: Continuous output at 1920×1080 @ 60 fps without frame drops or jitter under steady-state thermal conditions.
2. **Hardware Acceleration**: MediaCodec hardware encoder active with direct Surface input (zero CPU frame readbacks).
3. **Audio-Video Synchronization**: Consistent synchronization across streaming and recording sessions.
4. **Thermal and System Stability**: Predictable CPU and GPU utilization without severe thermal throttling.

### Verification Procedure

1. **Output and Scene Setup**:
   * Set Base Canvas and Output Resolution to `1920×1080` with FPS set to `60`.
   * Configure streaming output to `Android MediaCodec H.264` at 6000 kbps CBR.
   * Configure recording output to `Android MediaCodec HEVC (H.265)` with Hybrid MP4 container.
   * Add a dynamic scene with continuous motion (e.g., animated timers, scrolling text, or motion video).
2. **Continuous Testing**:
   * Execute continuous recording and RTMP streaming for at least 60 seconds.
   * Capture encoder logs via ADB:
     ```sh
     adb logcat -v threadtime -d -s 'OBS-MediaCodec:*' > mediacodec-test.log
     ```
   * Confirm that `event=started` verifies hardware component loading and that `submitted`, `output`, and `delivered` counters increment monotonically.
3. **Stream Analysis**:
   * Transfer the recorded MP4 file to a host computer and run verification:
     ```sh
     python scripts/verify-1080p60.py recording.mp4
     ```
   * The tool uses `ffprobe` and `ffmpeg` to verify packet timestamps (target ~16.6 ms delta), check for monotonic DTS progression, and validate stream integrity.

---

<a name="简体中文"></a>
## 简体中文

本文档定义了 OBS for Android 在 1080p60 规格下进行持续流媒体输出与录制的验收标准及验证流程。

### 验收指标

1. **画面规格**：分辨率 1920×1080，帧率稳定在 60 fps（稳态温控下无异常掉帧或卡顿）。
2. **硬件加速**：使用系统 MediaCodec 硬件加速组件，Surface 直连输入，无整帧 CPU 回读。
3. **音画同步**：持续测试期间音频与视频保持严格同步，无漂移或撕裂。
4. **系统负载**：CPU / GPU 占用维持在合理区间，长时间运行无持续发热导致的严重降频。

### 验证流程

1. **输出与场景配置**：
   * 基础画布与输出分辨率设为 `1920×1080`，FPS 设为 `60`。
   * 推流选用 `Android MediaCodec H.264`，码率设为 6000 kbps CBR。
   * 录像选用 `Android MediaCodec HEVC (H.265)`，格式选择 `Hybrid MP4`。
   * 添加包含持续运动元素（如秒表计时器、高动态画面）的测试源，确保画面内容具备连续的时间变化。
2. **持续录制与推流测试**：
   * 启动持续录制或本地 RTMP 推流至少 60 秒。
   * 通过 ADB 采集编码器日志：
     ```sh
     adb logcat -v threadtime -d -s 'OBS-MediaCodec:*' > mediacodec-test.log
     ```
   * 检查日志中 `event=started` 是否确认硬件组件已挂载，且 `event=progress` 中提交帧数与输出帧数单调同步递增。
3. **输出文件流分析**：
   * 将生成的 MP4 文件传至电脑，使用验证脚本对码流进行完整性检验：
     ```sh
     python scripts/verify-1080p60.py recording.mp4
     ```
   * 该工具利用 `ffprobe` 和 `ffmpeg` 解析视频流：验证解码帧总数与 DTS 时间戳间隔是否稳定在 ~16.6ms，并检查时间戳单调性与码流完整度。
