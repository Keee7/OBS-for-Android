<div align="center">

  <p align="center">
    <a href="https://obsproject.com" target="_blank" rel="noopener noreferrer">
      <img src="assets/obs-logo.png" width="60" height="60" alt="OBS Studio Logo" style="vertical-align: middle;" />
    </a>
    &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
    <a href="https://www.android.com" target="_blank" rel="noopener noreferrer">
      <img src="assets/android-logo.svg" width="60" height="60" alt="Android Logo" style="vertical-align: middle;" />
    </a>
  </p>

  <h1>OBS for Android</h1>

  <p>
    <b>Live Streaming &amp; Recording Studio for Android</b><br />
    <sub>Powered by OBS Studio 33 · MediaCodec Hardware Acceleration · OpenGL ES · Adaptive Landscape UI</sub>
  </p>

  <p>
    <a href="https://github.com/Keee7/OBS-for-Android/releases"><img src="https://img.shields.io/github/v/release/Keee7/OBS-for-Android?style=flat-square&color=2563EB&label=Release" alt="Release" /></a>
    <a href="https://github.com/Keee7/OBS-for-Android/actions/workflows/build-apk.yml"><img src="https://img.shields.io/github/actions/workflow/status/Keee7/OBS-for-Android/build-apk.yml?branch=main&style=flat-square&label=Build" alt="Build Status" /></a>
    <img src="https://img.shields.io/badge/Platform-Android%2010%2B%20(API%2029%2B)-3DDC84?style=flat-square&logo=android&logoColor=white" alt="Platform" />
    <img src="https://img.shields.io/badge/Arch-arm64--v8a-4F46E5?style=flat-square" alt="Architecture" />
    <img src="https://img.shields.io/badge/OBS%20Studio-v33.x-302E31?style=flat-square&logo=obsstudio&logoColor=white" alt="OBS Baseline" />
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPL--2.0--or--later-10B981?style=flat-square" alt="License" /></a>
  </p>

  <p>
    <a href="README.md"><b>English</b></a> &nbsp;•&nbsp;
    <a href="README_CN.md"><b>简体中文</b></a> &nbsp;•&nbsp;
    <a href="https://github.com/Keee7/OBS-for-Android/releases"><b>Download APK</b></a>
  </p>

</div>

---

An independent community Android port of [OBS Studio](https://github.com/obsproject/obs-studio) (`v33` baseline), built upon [cunminF/OBS_for_Android](https://github.com/cunminF/OBS_for_Android). It brings full scene compositing, multi-source capture, local Hybrid MP4/MOV recording, and live streaming to Android phones and tablets in a hardware-accelerated landscape studio interface.

## Interface Preview

<div align="center">

<p><b>Tablet</b> — Full multi-dock workspace</p>

<img src="assets/screenshot-tablet.webp" alt="OBS for Android Tablet Landscape Workspace" width="92%" />

<br /><br />

<p><b>Phone</b> — Adaptive compact landscape layout</p>

<img src="assets/screenshot-phone.webp" alt="OBS for Android Phone Compact Landscape Layout" width="92%" />

</div>

## Features

### Video, HDR & Encoding

| Capability | Details |
| --- | --- |
| **MediaCodec Hardware Encoding** | H.264, HEVC (including Main10 HDR on Android 13+), AV1 (streaming &amp; Hybrid MP4), and VP9 (Hybrid MP4) with zero-copy Surface input (`CBR` / `VBR` / `CQ`). |
| **Color Range & Fallbacks** | Full &amp; Limited (`Partial` / `Full`) color range probing per colorspace and rate mode, with automatic ByteBuffer YUV (`NV12` / `I420`) and software encoder fallback when hardware constraints apply. |
| **Software Encoders** | Bundled `obs_x264`, `ffmpeg_x265` (`3.6`, 8/10-bit), and `ffmpeg_aom_av1` (`3.10.0`) with matched rate-control mapping. |
| **10-bit HDR Pipeline** | Rec. 2100 PQ/HLG compositing with `P010` hardware decode/encode, `I010` software encoding, and Hybrid MP4 color metadata tagging. |

### Capture, Audio & Playback Sources

| Capability | Details |
| --- | --- |
| **Screen & Single-App Capture** | Zero-copy MediaProjection GPU capture (`SurfaceTexture`/OES, optional CPU fallback), Android 14+ single-app resize tracking, and Straight/Premultiplied Alpha compositing. |
| **WebView Browser Source** | Android System WebView GPU capture for transparent overlays &amp; danmaku widgets, local HTML/custom CSS, touch interaction, and "Control Audio via OBS" mixer routing. |
| **USB & Camera Sources** | Dual-backend USB UVC capture (Built-in Native + Android-hardened `libuvc 0.0.9`; `YUYV`, `UYVY`, `NV12`, `P010`, `RGB24`, `BGR24`, `MJPEG`, large-payload Bulk cameras, auto-matched UAC audio &amp; watchdog recovery), built-in Camera2 NDK, and Nikon Z-series PTP Live View. |
| **NDI® LAN I/O (BYOR)** | Native NDI® Input Source (`NsdManager` + manual IP discovery) and Main Program NDI® Output via user-imported official Android `libndi.so`. |
| **Media & Network Streams** | MediaCodec hardware decoding (`H.264`, `HEVC`, `VP8`, `VP9`, `AV1`) with software fallback; Document URI playback and `HTTP(S)`, `HLS`, `DASH`, `RTSP(S)`, `RTMP(S)`, `SRT`, `RIST`, `UDP`/`RTP`/`TCP` feeds. |
| **AAudio Input** | Low-latency native AAudio capture for built-in microphones, wired headsets, and USB audio interfaces / capture cards. |

### Android Architecture & UI

| Capability | Details |
| --- | --- |
| **Adaptive Phone & Tablet UI** | Qt `6.12` responsive workspace switching seamlessly between full tablet docks and compact phone layout (`< 952×552` logical px), with split-screen, freeform window, and IME support. |
| **Efficient 60 FPS UI Rendering** | Partial Android RHI backingstore dirty-rectangle coalescing and snapshot-based `VolumeMeter` / `MediaControls` / `AbsoluteSlider` updates to minimize frame drops and CPU load. |
| **Shared Storage Recording** | MediaStore / SAF file descriptor bridge for direct Hybrid MP4/MOV recording into public folders such as `Movies/OBS` or `Download/obs`. |

## Requirements

* **Architecture & OS**: `arm64-v8a`, Android 10+ (API 29+), Target SDK 35 (Compile SDK 36).
* **Dependencies**: Android System WebView (required for Browser Source). Supported hardware codecs and 10-bit HDR render targets depend on the device SoC and GPU driver.

## Quick Start

1. **Install**: Download an APK from [Releases](https://github.com/Keee7/OBS-for-Android/releases) and install via `adb install -r path/to/obs-android.apk`, or build from source following [BUILDING.md](BUILDING.md).
2. **Baseline Setup**:
   * **Video**: `1920×1080` @ `60 FPS`, `SDR BT.709` (for 10-bit HDR, choose `Rec. 2100 (PQ)` or `Rec. 2100 (HLG)` in **Settings → Advanced → Video** with `P010` + `MediaCodec HEVC Main10`, or `I010` + `x265` / `libaom`).
   * **Output**: `Android MediaCodec H.264` (`CBR`) for streaming; `Hybrid MP4` targeting shared storage (e.g. `/storage/emulated/0/Movies/OBS` or `Download/obs`) for recording.
3. **Add Sources**:
   * Add **Screen Capture (MediaProjection)**, **UVC Camera (Android USB)**, **Built-in Camera (Camera2)**, **Nikon Webcam**, or **Android Browser**.
   * For network streams, add **Media Source**, uncheck **Local File**, and enter an `HTTP(S)`, `HLS`, `DASH`, `RTSP`, `RTMP`, `SRT`, or `RIST` URL (pass FFmpeg flags such as `rtsp_transport=tcp` in **FFmpeg Options** if needed).

<details>
<summary><b>Optional: Verify Hardware Encoding via ADB Logcat</b></summary>

```sh
adb logcat -v threadtime -d -s 'OBS-MediaCodec:*'
```

Look for `event=started hardware=true software=false input=Surface`, `event=first-packet headers=valid keyframe=1`, and rising frame/byte counters in `event=summary` / `event=progress`.

</details>

## Known Limitations

* **HDR / 10-bit**: Requires GPU support for 16-bit normalized textures and floating-point render targets. Preview output and Android screen capture composite as SDR at the configured SDR white level; P010 USB input requires compatible capture hardware.
* **Browser Source**: Multi-touch gestures are not yet supported; routing cross-origin page audio requires CORS headers from the remote server.
* **Streaming Services**: Twitch and YouTube OAuth logins are disabled; configure RTMP server URLs and stream keys manually via **Custom** service.

## Documentation & License

* **User Guide**: [docs/tutorial/en/README.md](docs/tutorial/en/README.md) ([中文教程](docs/tutorial/README.md)) — Step-by-step setup, capture sources, streaming/recording, HDR/NDI, and troubleshooting
* **Build Guide**: [BUILDING.md](BUILDING.md) — GitHub Actions, Linux/WSL, and Windows build instructions
* **Architecture**: [IMPLEMENTATION_ANDROID.md](IMPLEMENTATION_ANDROID.md) — EGL/MediaCodec, MediaProjection, UVC, SAF storage, and Qt UI layers
* **Contributing**: [CONTRIBUTING.md](CONTRIBUTING.md) — Issue reporting and pull request guidelines
* **Notices**: [NOTICE.md](NOTICE.md) — Third-party licenses and trademark notices

Licensed under **GPL-2.0-or-later** ([LICENSE](LICENSE)). This is an independent community port and is not affiliated with the OBS Project.
