# Android Device Testing / Android 真机测试

[English](#english) | [简体中文](#简体中文)

<a name="english"></a>
## English

Run these commands from the repository root. Install a signed build with `adb install -r path/to/obs-android-release.apk`. When prompted, grant access to the screen, USB device or recording folder for the feature under test.

### Screen capture and layout

Add **Screen Capture (MediaProjection)** and approve the Android screen-sharing prompt. Verify GPU capture and preview alignment in fullscreen, split-screen and floating windows, including after dragging and resizing the window.

On Android 14+, select **A single app** in the system prompt to test transparent capture. Enable the target app's transparent background and place a contrasting background beneath the capture source. In the source properties, choose straight or premultiplied Alpha to match the target app. If Android delivers opaque frames, the background will remain opaque in OBS.

<a name="hardware-encoding"></a>
### Hardware encoding and 1080p60

1. In **Settings → Video**, set the canvas and output to 1920×1080 at 60 fps. Use SDR BT.709 and a scene with continuous motion, such as a video with a frame counter.
2. Use **Android MediaCodec H.264** with CBR for RTMP. To test HEVC recording, select **Android MediaCodec HEVC** in Advanced Output and choose Hybrid MP4.
3. Record for at least 60 seconds. Pull the recording to the host and run both checks with the corresponding codec. Install `ffmpeg` and `ffprobe` on the host for the file check.

   ```sh
   adb logcat -v threadtime -d -s 'OBS-MediaCodec:*' > mc.log
   python scripts/verify-mediacodec-log.py mc.log --codec hevc
   python scripts/verify-1080p60.py recording.mp4 --codec hevc
   ```

The encoder log identifies the hardware component, Surface input, first keyframe and frame counters. The file check looks for frame-rate shortfalls, gaps and repeated frames. During a longer run, check audio sync, rendering and encoding lag, and thermal throttling on the device.

### USB capture

Connect a USB 3 capture card, grant USB access, and choose 1920×1080 at 60 fps in the UVC source settings. Confirm SuperSpeed in the log. NV12 carries about 187 MB/s of raw pixels at this resolution and frame rate. YUYV carries about 249 MB/s.

With a moving HDMI signal, compare `cadence_input` (`same_luma_sample` and arrival intervals), `cadence_pts` (repeated or backward timestamps) and `cadence_select` (`no_frame_empty` and `skipped`) with OBS rendering and encoding lag. Briefly interrupt the HDMI signal and check whether the preview recovers.

### Shared storage recording

Choose Hybrid MP4 or Hybrid MOV and set the destination to `/storage/emulated/0/Download/obs`, or grant access to a folder through the Android picker. Play back two recordings to confirm that both files were saved. Restart the app and record again to check that directory access persists.

### Reference results

On the reference Android 16 device, a 1080p60 MediaCodec H.264 stream targeting 6 Mbps CBR delivered 5,317 frames in 88.600 seconds at about 6.028 Mbps. The recording at the receiver decoded without errors, and the largest DTS gap was 17 ms. One rendering lag frame and one encoding lag frame were recorded. The stream reconnected in about 1.267 seconds after a simulated receiver disconnection.

In the same local test setup, the encoder averaged 1.711 Mbps over a five-second window with the connection capped at 2 Mbps, then recovered to about 5.836 Mbps after the cap was lifted. The USB capture run reached about 1080p60 and 186.8 MB/s with NV12 over SuperSpeed after recovery.

---

<a name="简体中文"></a>
## 简体中文

在仓库根目录执行以下命令。用 `adb install -r path/to/obs-android-release.apk` 安装已签名的构建，并按测试项目授予录屏、USB 设备或录像目录权限。

### 屏幕采集和布局

添加“屏幕采集（MediaProjection）”源并同意系统共享请求。检查 GPU 采集和预览对齐情况，覆盖全屏、分屏和悬浮小窗，以及小窗拖动和缩放。

Android 14 及以上系统可在共享弹窗里选择“单个应用”测试透明叠加。开启目标应用的透明背景，在采集源下方放置对比明显的背景。按目标应用的输出选择直通或预乘 Alpha。若 Android 交付的是不透明帧，OBS 中的背景仍会保持不透明。

<a name="hardware-encoding-cn"></a>
### 硬件编码和 1080p60

1. 在“设置 → 视频”中将画布与输出设为 1920×1080、60 fps。使用 SDR BT.709 和持续运动的测试素材，例如带帧号的视频。
2. RTMP 推流使用 **Android MediaCodec H.264** 和 CBR。测试 HEVC 录像时，在高级输出模式下选择 **Android MediaCodec HEVC** 和 Hybrid MP4。
3. 录制至少 60 秒，把录像复制到电脑，用与实际编码一致的参数运行下列命令。文件检查需要电脑安装 `ffmpeg` 和 `ffprobe`。

   ```sh
   adb logcat -v threadtime -d -s 'OBS-MediaCodec:*' > mc.log
   python scripts/verify-mediacodec-log.py mc.log --codec hevc
   python scripts/verify-1080p60.py recording.mp4 --codec hevc
   ```

编码日志可核对硬件组件、Surface 输入、首个关键帧和帧计数。文件检查会报告帧率不足、帧间隔过大和画面重复。长时间测试时，在设备上同时检查音画同步、渲染和编码滞后以及温控降频。

### USB 采集卡

连接 USB 3 采集卡并授权，在 UVC 源属性里选择 1920×1080、60 fps，再从日志确认链路进入 SuperSpeed。该规格下，NV12 的原始像素吞吐约为 187 MB/s，YUYV 约为 249 MB/s。

使用持续运动的 HDMI 输入，将 `cadence_input` 的 `same_luma_sample` 和到帧间隔、`cadence_pts` 的时间戳变化，以及 `cadence_select` 的 `no_frame_empty` 和 `skipped` 与 OBS 渲染和编码滞后对应起来。短暂中断 HDMI 信号，检查恢复后预览是否自动继续。

### 共享目录录像

选择 Hybrid MP4 或 Hybrid MOV，将录像目录设为 `/storage/emulated/0/Download/obs`，也可通过 Android 文件选择器授权其他目录。连续录制两次并播放确认两个文件均已保存，重启应用后再录一次以检查目录授权。

### 参考设备实测

在参考 Android 16 设备上，用 MediaCodec H.264 以 1080p60、6 Mbps CBR 推流。接收端在 88.600 秒内收到 5,317 帧，平均约 6.028 Mbps，接收端录像可无错解码，最大 DTS 间隔为 17 ms。OBS 各记录了一帧渲染滞后和编码滞后，模拟接收端断开后约 1.267 秒恢复连接。

同一本地测试环境中，链路限速至 2 Mbps 时，编码器在 5 秒内平均输出约 1.711 Mbps。解除限速后回升至约 5.836 Mbps。USB 采集恢复后，SuperSpeed 链路下的 NV12 采集达到约 1080p60、186.8 MB/s。
