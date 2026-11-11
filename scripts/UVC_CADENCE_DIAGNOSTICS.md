# UVC Cadence and Frame Delivery Diagnostics / UVC 帧节拍与到帧抖动诊断指南

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This document describes diagnostic methodologies for measuring frame pacing, arrival jitter, and selection cadence in the UVC video ingestion pipeline.

### 1. Key Diagnostic Metrics

* `event=cadence_input`: Reports completed raw YUYV/NV12 frame statistics over five-second windows.
  * `same_luma_sample`: Sparse luma fingerprint comparisons across consecutive frames, used as an indicator of potential duplicate frames.
  * `dt_*`: Binned arrival time deltas derived from USB transfer completion timestamps to quantify ingestion jitter.
* `event=cadence_select`: Reports OBS video tick processing:
  * `selected`: Successfully dequeued frame count.
  * `no_frame_empty`: Ticks where the input queue was empty.
  * `skipped`: Queued frames discarded to maintain synchronization.
* `event=cadence_pts`: Tracks UVC Presentation Time Stamp (PTS) progression and checks for monotonic sequencing.

### 2. Diagnosis Workflow

1. Supply a continuous motion HDMI input with frame numbers or timestamps.
2. Record for at least 30 seconds and inspect the aggregated metrics in the log.
3. Correlate input arrival cadence with OBS rendering/encoding lag statistics to isolate bottlenecks between USB transfer, frame queueing, and encoder consumption.

---

<a name="简体中文"></a>
## 简体中文

本文档介绍在 UVC 视频输入管线中分析到帧节拍（Cadence）、时间戳抖动与 OBS 选帧行为的诊断方法。

### 1. 核心诊断指标

* `event=cadence_input`：以大约 5 秒为窗口统计完成的 Raw YUYV/NV12 帧接收状态：
  * `same_luma_sample`：稀疏亮度指纹对比计数，用于辅助识别潜在的相邻重复帧；
  * `dt_*`：基于 USB 传输完成时间戳的到帧间隔分布直方图，用于量化摄入抖动。
* `event=cadence_select`：统计 OBS 视频 Tick 轮询中的处理情况：
  * `selected`：成功出队交付给合成器的帧数；
  * `no_frame_empty`：输入队列为空的 Tick 计数；
  * `skipped`：为维持时钟同步而丢弃的积压帧数。
* `event=cadence_pts`：记录 UVC 硬件 Presentation Time Stamp (PTS) 的递增连续性与单调性。

### 2. 诊断与分析流程

1. 输入包含连续变动画面（如带帧号或高精度计时器）的 HDMI 信号源；
2. 保持稳定录制至少 30 秒，并在日志中提取聚合统计；
3. 将 USB 到帧节拍、OBS 选帧统计与编码器渲染滞后指标联合比对，准确定位性能瓶颈位于 USB 传输层、队列调度层还是编码管线。
