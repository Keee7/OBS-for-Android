# Reference Benchmark: Local RTMP Streaming and Window Mode Stability / 参考基准报告：本地 RTMP 推流与多窗口稳定性验证

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This report documents benchmark results for local RTMP streaming and multi-window coordinate stability on the reference Android platform.

### 1. Window Mode Geometry Alignment

Standardized all window coordinate calculations onto `getLocationInWindow`:
* **Transitions Verified**: Fullscreen → Floating small window → Window dragging → Resizing → Return to fullscreen.
* **Result**: Video preview coordinates align precisely with OBS canvas containers without obscuring controls or scene lists.

### 2. RTMP Streaming and Rate Control Stability

Streaming targets local RTMP loopback at 1920×1080 @ 60 fps, 6 Mbps CBR, using MediaCodec H.264 hardware encoding:

| Test Item | Measured Benchmark |
| --- | --- |
| **Normal Ingestion** | 88.600 seconds, 5,317 video frames; measured 60.000 fps, video bitrate ~6.028 Mbps |
| **Timestamp Continuity** | Monotonic DTS progression, max frame delta 17 ms, initial packet is keyframe |
| **Stream Integrity** | Complete FFmpeg decode with zero decode errors; 3,817 AAC audio packets in sync |
| **Encoder Queueing** | `failed=0`; encoding and rendering lag limited to single transient events |
| **Receiver Reconnection** | Simulated receiver drop; automatic reconnection completed in ~1.267 seconds |
| **Dynamic Bitrate Throttling** | Throttling to 2 Mbps reduced output to ~1.711 Mbps over 5s window; recovered smoothly to ~5.836 Mbps once throttling was lifted |

---

<a name="简体中文"></a>
## 简体中文

本报告记录了在参考 Android 平台上进行的本地 RTMP 实时推流与多窗口几何对齐基准测试结果。

### 1. 窗口几何坐标对齐测试

将所有父容器与预览坐标统一采用 `getLocationInWindow` 计算：
* **验证路径**：全屏 → 进入悬浮小窗 → 窗口拖拽移动 → 自由缩放尺寸 → 恢复全屏；
* **测试结果**：预览画面始终与 OBS 画布容器严格对齐，未遮挡控制按钮与场景列表。

### 2. RTMP 推流与码率控制稳定性

测试配置为 1920×1080 @ 60 fps、6 Mbps CBR，使用 MediaCodec H.264 硬件编码器推流至本地 RTMP 服务：

| 测试项 | 实测基准数据 |
| --- | --- |
| **常规持续推流** | 持续 88.600 秒，接收 5,317 帧，测得 60.000 fps，平均码率约 6.028 Mbps |
| **时间戳连续性** | DTS 单调递增，最大帧间隔 17 ms，首包为合法关键帧 |
| **码流完整性** | FFmpeg 完整解码退出码为 0，无任何音视频解码错误；AAC 音频包正常同步 |
| **编码队列稳定性** | 会话错误计数 `failed=0`；持续推流期间仅记录到单次瞬时渲染/编码微滞后 |
| **异常断线自愈** | 模拟服务端断开，客户端在约 1.267 秒内自动完成状态重置与重新连接 |
| **动态码率调节** | 本地限速 2 Mbps 时，编码输出在 5 秒窗口内平滑降至约 1.711 Mbps；限速解除后平滑回升至 ~5.836 Mbps |
