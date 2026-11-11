# UVC Ingestion Pipeline and CPU Utilization Analysis / UVC 数据收包与 CPU 占用分析

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This document analyzes the CPU utilization, memory throughput, and optimization strategies for high-bandwidth UVC video capture on Android.

### 1. Ingestion Pipeline & Existing Optimizations

* **Decoupled libusb Worker Thread**: Isolates libusb bulk transfer completions from frame assembly and OBS video submission. Pre-allocated buffer pools transfer chunk ownership cleanly.
* **Standard Bulk Raw Reassembly**: For devices adhering to standard UVC payload boundaries, frames are assembled directly without intermediate byte-stream scanning.
* **Bounded Queueing & Overload Drop**: Bounded buffers with sequence gap detection isolate partial frames under system load, preventing corrupt frame handoffs.
* **GPU Direct Upload**: YUYV / NV12 buffers are uploaded directly to GPU textures, avoiding per-pixel CPU colorspace conversions.

### 2. Throughput and Memory Copy Modeling

Estimated uncompressed pixel throughput at 1920×1080 @ 60 fps:

| Metric | YUYV (4:2:2) | NV12 (4:2:0) |
| --- | ---: | ---: |
| Pixel Payload Throughput | 248.832 MB/s | 186.624 MB/s |
| 128 KiB Transfer Completion Rate | ~1,898 / sec | ~1,424 / sec |
| 64 KiB Transfer Completion Rate | ~3,797 / sec | ~2,848 / sec |

### 3. Optimization Priorities

1. **Consolidated Retry Checks**: Merged event loop retry slot checks into single atomic bitmask handoffs, reducing mutex contention under normal traffic.
2. **Format Selection**: Preferring NV12 when supported by the capture hardware reduces raw memory and USB bandwidth by 25% compared to YUYV.
3. **Buffer Budgeting**: Sizing pre-allocated spare buffers to balance latency against memory footprint.

---

<a name="简体中文"></a>
## 简体中文

本文档分析了在 Android 设备上进行高带宽 UVC 视频采集时的 CPU 占用、内存吞吐及关键优化路径。

### 1. 收包管线与已有优化

* **独立 libusb 事件线程**：将 USB bulk 传输事件完成回调与图像帧组装及 OBS 视频提交解耦，通过预分配缓冲池交接所有权，避免在中断线程中产生内存拷贝；
* **标准 Raw 载荷直接解析**：对符合标准 UVC payload 边界的设备，直接按载荷块重组目标帧，免去中间流缓冲与逐字节扫描；
* **有界队列与丢帧保护**：预留有界缓冲，当系统负载过高发生包序号缺口时，安全丢弃破损半帧，避免拼接伪帧；
* **GPU 直接纹理上传**：保持 YUYV / NV12 格式直达 GPU 纹理采样着色器，消除 CPU 逐像素色彩空间转换开销。

### 2. 吞吐量与内存拷贝分析

在 1920×1080 @ 60 fps 下，未压缩原始像素数据吞吐量测算：

| 评估指标 | YUYV (4:2:2) | NV12 (4:2:0) |
| --- | ---: | ---: |
| 纯像素载荷吞吐 | 248.832 MB/s | 186.624 MB/s |
| 满 128 KiB 传输完成频率 | 约 1,898 次/秒 | 约 1,424 次/秒 |
| 满 64 KiB 传输完成频率 | 约 3,797 次/秒 | 约 2,848 次/秒 |

### 3. 性能优化重点

1. **合并事件循环重试检测**：将原先每轮事件循环的逐槽互斥锁扫描，优化为单次原子重试状态位图交接，显著降低正常收包状态下的锁竞争；
2. **优先选用 NV12 格式**：在采集卡支持的前提下，NV12 比 YUYV 节省 25% 的 USB 传输量与内存搬移带宽；
3. **合理配置缓冲池配额**：通过合理的 spare 缓冲池预算平滑输入时钟抖动，兼顾内存占用与流畅度。
