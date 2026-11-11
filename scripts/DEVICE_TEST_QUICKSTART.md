# Device Testing Quickstart Guide / 移动端真机快速测试指引

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This guide provides standardized steps for deploying, configuring, and verifying the core features of OBS for Android on physical devices.

### 1. Deployment and Installation

1. **Install APK**:
   ```sh
   adb install -r <obs-android-release.apk>
   ```
   > If upgrading from an existing build with a different signing key, back up configurations before performing a clean reinstall.
2. **Grant Permissions**:
   Upon launching the app, grant necessary permissions for storage access, foreground services, and floating window overlays as requested.

### 2. Core Feature Verification Checklist

#### 2.1 Screen Capture & Landscape UI
1. Add a "Screen Capture (MediaProjection)" source and approve the system screen recording dialog.
2. Verify that "GPU Screen Capture" is active by default.
3. **Landscape Adaptation**: On a smartphone in landscape orientation, verify that the compact layout activates automatically (32px row heights, empty toolbars collapsed). On a tablet, verify the spacious touch layout (48px rows). Test split-screen and floating window transitions.

#### 2.2 Hardware Encoding
1. Open "Settings → Output", set Streaming Encoder to `Android MediaCodec H.264` and Recording Encoder to `Android MediaCodec HEVC`.
2. Configure the recording path to `/storage/emulated/0/Download/obs`.
3. Record a 30-second session and verify via ADB:
   ```sh
   adb logcat -v threadtime -d -s 'OBS-MediaCodec:*' > mc.log
   python scripts/verify-mediacodec-log.py mc.log --codec h264
   ```

#### 2.3 UVC Video Ingestion
1. Connect a USB 3.0 UVC capture card via an OTG adapter and grant USB permissions.
2. Add a "UVC Camera" source and select `NV12` or `YUYV` at `1920×1080 @ 60fps`.
3. Verify smooth live preview and check that a SuperSpeed connection was negotiated in the device log.

---

<a name="简体中文"></a>
## 简体中文

本文档提供在 Android 真实设备上部署、配置与验证 OBS for Android 核心特性的标准化步骤。

### 1. 部署与安装

1. **安装 APK**：
   ```sh
   adb install -r <obs-android-release.apk>
   ```
   > 若更换签名或版本升级提示签名冲突，请先备份配置后再执行覆盖安装。
2. **授予系统权限**：
   启动应用后，根据使用场景授予媒体读写、前台服务以及悬浮窗权限。

### 2. 核心功能验证清单

### 2.1 屏幕采集与横屏自适应体验
1. 新建“屏幕采集（MediaProjection）”来源，在系统弹窗中同意录屏授权；
2. 确认默认启用“GPU 屏幕采集”模式；
3. **横屏自适应测试**：手机横屏握持下，界面自动进入紧凑模式（32px 行高，空闲工具栏自动折叠）；在平板大屏上展开为宽松触控模式（48px 行高）。测试分屏与小窗切换，确认画面预览无错位。

### 2.2 硬件编解码验证
1. 打开“设置 → 输出”，推流编码器选择 `Android MediaCodec H.264`，录像编码器选择 `Android MediaCodec HEVC`；
2. 录像路径配置为 `/storage/emulated/0/Download/obs`；
3. 进行 30 秒录制，通过 ADB 执行快速日志校验：
   ```sh
   adb logcat -v threadtime -d -s 'OBS-MediaCodec:*' > mc.log
   python scripts/verify-mediacodec-log.py mc.log --codec h264
   ```

### 2.3 UVC 采集卡摄入
1. 将 USB 3.0 UVC 采集卡通过 OTG 转接线连接至设备，并同意 USB 设备访问权限；
2. 添加“UVC 摄像头”来源，手动选择 `NV12` 或 `YUYV`、`1920×1080 @ 60fps`；
3. 观察实时预览画面是否流畅低延迟，并在日志中确认 SuperSpeed 链路已成功协商。
