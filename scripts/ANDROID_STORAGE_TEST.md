# Scoped Storage Recording Guide / Android 分区存储与共享目录录像指南

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This document describes recording workflows compliant with Android Scoped Storage and Storage Access Framework (SAF) requirements.

### 1. Storage Architecture & Direct I/O

Traditional POSIX file paths face permission restrictions in shared Android directories. This implementation adopts the following architecture:

* **Direct File Descriptor Streaming**: Creates recording files in public directories (e.g., `Download/`, `Movies/`) using MediaStore or SAF DocumentsContract APIs. The OBS MP4 buffered writer streams directly to the granted file descriptor, packaging files in-place without requiring full intermediate copies.
* **Storage Capacity Probing**: Queries storage volume capacity using Android's `StatFs` service with a 50 MiB low-space protection floor, avoiding POSIX `statvfs` false alarms.

### 2. Configuration & Verification Procedure

1. **Configure Output Path**:
   * Navigate to **Settings → Output → Recording**.
   * Set Recording Format to **Hybrid MP4** or **Hybrid MOV**.
   * Enter the destination path: `/storage/emulated/0/Download/obs` (or select a directory using the system picker).
2. **Multi-Session Verification**:
   * Perform a standard 60-second recording and verify that the file plays cleanly.
   * Start and stop recording twice in quick succession to verify that unique sequence filenames are generated without overwriting previous sessions.
   * Restart the application and verify that recording succeeds without re-requesting permissions.

---

<a name="简体中文"></a>
## 简体中文

本文档介绍如何在遵循 Android 分区存储（Scoped Storage）规范的前提下，进行本地视频录制。

### 1. 存储架构与直写设计

传统的 POSIX 路径与文件 API 在 Android 10+ 的共享目录中受限。为实现高效录制，本工程采用了以下设计：

* **原生文件描述符直写**：通过 MediaStore API 在公共媒体目录（如 `Download/`、`Movies/`）创建录像目标文件，OBS 缓冲写入器直接操作底层文件描述符，原地完成 MP4 封装，无需在临时私有目录下完整转存；
* **容量检测容错**：采用 Android 底层 `StatFs` 服务查询存储卷剩余容量，设定 50 MiB 安全阈值，避免传统 `statvfs` 误报 0 导致录像意外中止。

### 2. 推荐配置与测试流程

1. **设置目标路径**：
   * 打开 **设置 → 输出 → 录像**；
   * 录像格式选择 **Hybrid MP4** 或 **Hybrid MOV**；
   * 录像路径输入 `/storage/emulated/0/Download/obs`（或通过系统目录选择器授权指定文件夹）。
2. **多段测试**：
   * 执行单次 60 秒常规录制，确认录制结束时文件正常写入且可完整播放；
   * 连续快速启动/停止录制两次，验证系统生成不同序列文件名且不覆盖旧录像；
   * 重启应用后再次录制，验证跨进程生命周期的存储授权依然有效。
