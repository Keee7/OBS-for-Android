# High-Speed UVC Ingestion and Pipeline Optimization / USB 3.0 UVC 采集卡高速直通与性能优化

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This guide describes the high-performance ingestion pipeline for USB 3.0 / SuperSpeed UVC capture cards operating at 1920×1080 @ 60 fps (YUYV or NV12) with direct MediaCodec H.264 / HEVC hardware encoding.

### 1. Bandwidth and Protocol Considerations

Uncompressed 1080p60 video requires significant sustained throughput:
* **YUYV 4:2:2 (8-bit)**: ~248.8 MB/s (~1.99 Gbps raw pixel bandwidth).
* **NV12 4:2:0 (8-bit)**: ~186.6 MB/s (~1.49 Gbps raw pixel bandwidth; 25% lower data volume).

To support this bandwidth reliably, the capture card must be connected to a USB 3.0 port and negotiate SuperSpeed mode.

### 2. Pipeline Optimizations

* **Decoupled Worker Threads**: Completely separates `libusb` event completion callbacks from frame parsing and OBS submission. Uses pre-allocated ring buffer pools with ownership handoffs, avoiding memory copies in interrupt contexts.
* **Direct Raw YUV Reassembly**: Extracts raw YUV frames directly from standard bulk payload streams using negotiated packet boundaries, avoiding intermediate byte-stream buffering and CPU pixel scanning.
* **Direct GPU Upload**: YUYV / NV12 frames are uploaded straight into OBS GPU texture conversion shaders (BT.709 color matrix) without intermediate BGRA CPU conversions.
* **Automatic Recovery Watchdog**: Monitors continuous valid video frame cadence. If frame arrival stalls or USB endpoints stall, the watchdog triggers endpoint halts clearing and state renegotiation cleanly.

---

<a name="简体中文"></a>
## 简体中文

本文档介绍针对 USB 3.0 / SuperSpeed 1080p60 采集卡的数据接入管线、低延迟传输优化及系统负载控制策略。

### 1. 带宽模型与传输通道

在 1920×1080 @ 60 fps 规格下，未压缩视频具备极高的数据吞吐需求：
* **YUYV (4:2:2)**：原始数据量约为 248.8 MB/s（~1.99 Gbps）；
* **NV12 (4:2:0)**：原始数据量约为 186.6 MB/s（~1.49 Gbps），数据量减少 25%。

为了稳定支撑这一带宽需求，采集卡必须运行在 USB 3.0 SuperSpeed 模式下。

### 2. 数据泵解耦与性能优化

* **多线程管线解耦**：将 `libusb` 事件接收线程与 YUV 帧解包/重组线程完全分离；在接收端使用预分配的环形缓冲池（Buffer Pool），完成回调仅进行指针所有权交接，避免在中断上下文中发生数据拷贝。
* **Raw YUV 直接重组**：针对标准 bulk 载荷，严格依据 UVC 协议头部字段快速解出原始 YUV 像素包，直接组装成目标图像，消除中间字节流拷贝。
* **GPU 纹理直连**：YUYV / NV12 帧直接进入 OBS 的 GPU 纹理上传与色彩转换管线（BT.709 矩阵），不在 CPU 层面进行耗时的 BGRA 色彩格式转换。
* **断流检测与自愈机制**：引入基于有效视频帧率的看门狗服务；当检测到连续数秒无有效帧到达或 USB 端点卡死时，自动触发排空并执行重协商，保障流媒体长时间摄入的稳定性。
