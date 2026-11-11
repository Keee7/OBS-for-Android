<div align="center">

  <p align="center">
    <a href="https://obsproject.com" target="_blank" rel="noopener noreferrer">
      <img src="assets/obs-logo.png" width="58" height="58" alt="OBS Studio Logo" style="vertical-align: middle;" />
    </a>
    &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
    <a href="https://www.android.com" target="_blank" rel="noopener noreferrer">
      <img src="assets/android-logo.svg" width="58" height="58" alt="Android Logo" style="vertical-align: middle;" />
    </a>
  </p>

  <h1>OBS for Android</h1>

  <p>
    <b>High-Performance Live Streaming & Recording Studio for Android</b><br />
    <sub>Powered by OBS Studio 33 · Accelerated by MediaCodec & OpenGL ES · Responsive Landscape Architecture</sub>
  </p>

  <p>
    <a href="https://github.com/Keee7/OBS-for-Android/releases"><img src="https://img.shields.io/github/v/release/Keee7/OBS-for-Android?style=flat-square&color=2563EB&label=Release" alt="Release"></a>
    <a href="https://github.com/Keee7/OBS-for-Android/actions/workflows/build-apk.yml"><img src="https://img.shields.io/github/actions/workflow/status/Keee7/OBS-for-Android/build-apk.yml?branch=main&style=flat-square&label=Build" alt="Build Status"></a>
    <img src="https://img.shields.io/badge/Platform-Android%2010%2B%20(API%2029%2B)-3DDC84?style=flat-square&logo=android&logoColor=white" alt="Platform">
    <img src="https://img.shields.io/badge/Arch-arm64--v8a-4F46E5?style=flat-square" alt="Architecture">
    <img src="https://img.shields.io/badge/OBS%20Studio-v33.x-302E31?style=flat-square&logo=obsstudio&logoColor=white" alt="OBS Baseline">
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPL--2.0--or--later-10B981?style=flat-square" alt="License"></a>
  </p>

  <p>
    <a href="README.md"><b>English</b></a> &nbsp;•&nbsp;
    <a href="README_CN.md"><b>简体中文</b></a> &nbsp;•&nbsp;
    <a href="https://github.com/Keee7/OBS-for-Android/releases"><b>Download APK</b></a> &nbsp;•&nbsp;
    <a href="BUILDING.md"><b>Build Guide</b></a> &nbsp;•&nbsp;
    <a href="CONTRIBUTING.md"><b>Contributing</b></a>
  </p>

  <br />

  <p align="center">
    <img src="screenshot.png" alt="OBS for Android Preview" width="90%" style="max-width: 840px; border-radius: 10px; box-shadow: 0 8px 30px rgba(0,0,0,0.2);" />
  </p>

  <p align="center">
    <code>⚡ MediaCodec HW Encoding</code> &nbsp;•&nbsp;
    <code>📱 Responsive Landscape UI</code> &nbsp;•&nbsp;
    <code>🎥 Zero-Copy GPU Capture</code> &nbsp;•&nbsp;
    <code>🌐 WebView Browser Source</code> &nbsp;•&nbsp;
    <code>🔌 High-Speed UVC Ingestion</code>
  </p>

</div>

---

An Android port of OBS Studio providing scene composition, multi-source input, local recording, and RTMP live streaming.

Based on [cunminF/OBS_for_Android](https://github.com/cunminF/OBS_for_Android) and updated to the [OBS Studio 33](https://github.com/obsproject/obs-studio) upstream baseline. This fork introduces **MediaCodec hardware encoding and decoding, HEVC and AV1 software encoding, GPU screen capture, a WebView browser source, high-speed UVC capture ingestion, Android shared storage recording, and a responsive landscape layout engine across mobile phones and tablets**.

> **Open Source Notice**: This is a community-driven open source project licensed under **GPL-2.0-or-later**. It is not an official release from the OBS Project. Original commit history, author attributions, and licenses are fully preserved.

---

## Features

| Feature | Details |
| --- | --- |
| **MediaCodec Hardware Encoding** | Hardware H.264, HEVC (H.265), AV1, and VP9 encoding. Surface input avoids full-frame CPU readback. Simple mode lists CBR-capable encoders. Advanced mode exposes supported CBR, VBR and CQ options and hardware quality controls. AV1 works with compatible streaming services and Hybrid MP4. VP9 is available for Hybrid MP4 recording. |
| **Software HEVC & AV1** | Integrated x265 (3.6) and libaom (3.10.0) software encoders with independent presets for streaming and recording. |
| **GPU Screen Capture** | Android MediaProjection routed via SurfaceTexture/OES directly into the OBS composition pipeline, with retry backoff. CPU capture is available as a manual option. |
| **Window Capture with Alpha** | Android 14+ content-size callbacks with straight and premultiplied Alpha modes. Transparency depends on whether Android supplies Alpha in the captured frames. |
| **WebView Browser Source** | GPU texture integration of the Android System WebView with custom URLs, dimensions, frame rates, custom CSS, and touch interaction. Features a zero-copy ImageReader + EGLImage transparent background pipeline. |
| **Audio Routing** | AAudio engine supporting dynamic selection of built-in microphones, wired headsets, and USB audio / capture-card UAC sources. |
| **Hardware Video Decoding** | Android document URI playback using FFmpeg with hardware-accelerated MediaCodec decoders (H.264, HEVC, VP8, VP9, AV1), with software fallback during initialization, including libaom for AV1. |
| **Shared Storage Recording** | MediaStore and Storage Access Framework (SAF) integration for recording directly to shared folders such as `Download/obs` with Hybrid MP4/MOV, avoiding scoped storage quota false alarms. |
| **High-Speed UVC Ingestion** | Multi-threaded pipeline for USB 3.0 / SuperSpeed 1080p60 capture cards, bounded buffer handoff, raw YUV (YUYV/NV12) reassembly, and automated watchdog recovery. |
| **Responsive Landscape UI** | Dynamic breakpoint engine (`952×552` logical pixels) tailored for landscape interaction. Automatically adapts between a compact landscape mode for smartphones (32px touch rows, auto-collapsed idle toolbars) and a spacious touch layout for tablets (48px rows), based on window size, including split-screen and floating windows. |
| **Qt UI & IME Adaptations** | Unified window coordinate geometry; optimized virtual keyboard event handling, addressing repeated keyboard requests, cursor offsets and dialog close artifacts. |

---

## Form Factors & System Requirements

* **Architecture**: `arm64-v8a`
* **OS Version**: Android 10+ (API 29+), Target SDK 35
* **Form Factor Adaptation**:
  * **Smartphones**: In landscape orientation, automatically engages **compact landscape layout** with optimized 32px touch rows and auto-collapsing idle toolbars, maximizing canvas preview area on smaller screens.
  * **Tablets & Large Displays**: Expands into a **spacious touch layout** (48px rows) offering a desktop-grade workflow.
  * **Split-Screen & Floating Windows**: Dynamically transitions between compact and spacious layouts based on active window geometry, protected by a 48px hysteresis band against jitter.
* **Prerequisites**: Android System WebView for the browser source. Hardware codec options depend on the device.

---

## Reference Benchmarks

Verified on reference hardware (Android 16 mobile device, 1080p60 target output):

* **Local RTMP Streaming**: 1920×1080 at 60 fps with a 6 Mbps CBR target. The receiver logged 5,317 frames over 88.600 seconds at about 6.028 Mbps. OBS reported one frame of rendering lag and one frame of encoding lag.
* **Network Reconnection**: The simulated receiver disconnection recovered in approximately 1.267 seconds.
* **UVC Card Ingestion**: NV12 over SuperSpeed reached about 60 fps at 1080p after recovery, with about 186.8 MB/s measured throughput.

> **Note**: These measurements cover local test conditions. See the [RTMP test record](scripts/DEVICE_VALIDATION_WINDOW_STREAM.md) and [IME/UVC test record](scripts/IME_UVC_TEST.md) for the setup and scope.

---

## Getting Started

1. **Obtain the APK**: Download the latest build from [Releases](https://github.com/Keee7/OBS-for-Android/releases), or build from source following [BUILDING.md](BUILDING.md).
2. **Installation**:
   ```sh
   adb install -r path/to/obs-android.apk
   ```
3. **Recommended Initial Setup**:
   * **Video**: Set Base Canvas and Output Resolution to `1920×1080`, FPS to `60`, Color Space to `SDR BT.709`.
   * **Output (Streaming)**: Select `Android MediaCodec H.264` with `CBR` rate control.
   * **Recording**: Use `Hybrid MP4` and set the destination path to `/storage/emulated/0/Download/obs`.
4. **Adding Sources**:
   * Add a "Screen Capture" source and grant permission in the system prompt.
   * For capture cards, add a "UVC Camera" source and configure the desired YUV resolution and frame rate.

---

## Verifying Hardware Encoding

Verify that hardware encoding is active via ADB:

```sh
adb logcat -v threadtime -d -s 'OBS-MediaCodec:*' > mediacodec-log.txt
python scripts/verify-mediacodec-log.py mediacodec-log.txt --codec h264
```

Look for the following key indicators:
* `event=started hardware=true software=false input=Surface`
* `event=first-packet headers=valid keyframe=1`
* Monotonically increasing frame and byte counters in `event=progress`.

The log verifier supports H.264 and HEVC. For AV1 and VP9, inspect the session logs and confirm the recording codec with `ffprobe`. For details, see the [Hardware Encoder Guide](scripts/HEVC_HARDWARE_TEST.md).

---

## Known Limitations

* **HDR / 10-bit**: Limited to 8-bit SDR BT.709 pipelines at present.
* **Browser Audio & Gestures**: Multi-touch gestures and dedicated browser audio routing are not yet supported in the current browser source.
* **Service Integrations**: Direct Twitch/YouTube account OAuth integrations are disabled; please configure RTMP server URLs and stream keys manually.

---

## License and Attribution

Licensed under **GPL-2.0-or-later**. See [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md) for details.

* Based on [cunminF/OBS_for_Android](https://github.com/cunminF/OBS_for_Android).
* Thanks to the [OBS Studio](https://github.com/obsproject/obs-studio) team and all upstream contributors.
