# Frame Cadence and Recovery Engineering Report / 编码节拍与 UVC 恢复技术报告

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This technical report details the investigation and resolution of video delivery jitter, EGL surface vsync quantization, and UVC recovery pipelines.

### 1. Root Cause Analysis: Vsync Quantization

* **Issue Observed**: Video streaming sessions showed intermittent stepping where stable 60.0 fps throughput suddenly dropped to ~33.6 fps or exhibited alternating dips and spikes.
* **Root Cause**: The encoder EGL surface creation path previously lacked an explicit `eglSwapInterval(display, 0)` call. By default, EGL swap interval is 1, causing `eglSwapBuffers` to wait for the next display vsync. If a single frame misses its vsync deadline, video submission becomes quantized to 30 fps (half the refresh rate), depleting the GPU texture queue.
* **Resolution**: Immediately invoke `eglSwapInterval(display, 0)` upon encoder surface creation. Added instrumentation to monitor swap durations (`swap_avg_us` and `swap_max_us`) in progress logs.

### 2. UVC Ingestion Cadence and Recovery

* **Buffer Decoupling**: Separated raw format negotiation from the latest-only latency policy. By default, OBS timing buffers smooth out USB arrival jitter.
* **Bounded History**: Clamped timestamp tracking queues to a bounded capacity (4,096 entries) to prevent unbounded memory growth during frame drop scenarios.
* **Dynamic Bitrate Backoff**: When hardware encoders reject runtime bitrate change parameters, the pipeline retains the last accepted configuration and warns cleanly without crashing the session.

---

<a name="简体中文"></a>
## 简体中文

本技术报告详述了视频推流抖动、EGL 输入面 vsync 周期量化以及 UVC 恢复机制的排查与工程修复。

### 1. 根因分析：Vsync 周期量化问题

* **异常现象**：推流过程中出现阶段性帧率骤降（如由稳定 60.0 fps 突然跳变为 33.6 fps），伴随短暂码率峰值与编码滞后；
* **根因定位**：MediaCodec 输入面创建 Recordable EGL Surface 后未显式调用 `eglSwapInterval(display, 0)`。EGL 默认交换间隔为 1，`eglSwapBuffers` 会同步等待下一拍垂直同步信号（vsync）。若某帧略微错过边界，视频线程就会被强制量化至 30 fps，进而导致 GPU 纹理池耗尽；
* **修复方案**：建面后立即显式调用 `eglSwapInterval(display, 0)` 禁用垂直同步等待，并在日志中输出 `swap_avg_us` 与 `swap_max_us`（稳定 60 fps 时耗时仅为 1~2 ms）。

### 2. UVC 数据摄入节拍与恢复

* **时序缓冲解耦**：将 raw 格式协商与无缓冲策略解耦，默认启用 OBS 时序缓冲以平滑 USB 摄入时钟抖动；
* **有界历史队列**：将时间戳记录与状态跟踪限制在 4096 项的有界容量内，避免在异常丢帧时历史队列无界扩张；
* **动态码率退避容错**：当硬件编码器拒绝动态码率更新指令时，系统保留最后一次成功生效的码率，并记录警告，确保输出会话不因驱动层参数拒绝而异常中断。
