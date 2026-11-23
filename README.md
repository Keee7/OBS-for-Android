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
    <b>Live Streaming & Recording Studio for Android</b><br />
    <sub>Powered by OBS Studio 33 · MediaCodec Hardware Acceleration · OpenGL ES · Adaptive Landscape UI</sub>
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
    <a href="IMPLEMENTATION_ANDROID.md"><b>Architecture</b></a> &nbsp;•&nbsp;
    <a href="CONTRIBUTING.md"><b>Contributing</b></a>
  </p>

  <br />

  <p align="center">
    <img src="screenshot.png" alt="OBS for Android Preview" width="90%" style="max-width: 840px; border-radius: 10px; box-shadow: 0 8px 30px rgba(0,0,0,0.2);" />
  </p>

</div>

---

An Android port of [OBS Studio](https://github.com/obsproject/obs-studio) (`v33` baseline) built upon [cunminF/OBS_for_Android](https://github.com/cunminF/OBS_for_Android), delivering scene compositing, multi-source capture, local recording, and live streaming in an adaptive landscape interface.

## Features

| Capability | Summary |
| --- | --- |
| **MediaCodec Hardware Encoding** | H.264, HEVC (with Main10 HDR on Android 13+), AV1 (streaming & Hybrid MP4), and VP9 (Hybrid MP4) via zero-copy Surface input (`CBR` / `VBR` / `CQ`). |
| **Software Encoders** | Integrated `x264`, `x265` (`3.6`, 8/10-bit), and `libaom` (`3.10.0`) AV1. |
| **10-bit HDR Pipeline** | Rec. 2100 PQ/HLG compositing with `P010` hardware decode/encode, `I010` software encoding, and Hybrid MP4 color metadata. |
| **Screen & Window Capture** | Zero-copy MediaProjection GPU capture (`SurfaceTexture`/OES, with optional CPU fallback), Android 14+ single-app resize, and Straight/Premultiplied Alpha compositing. |
| **WebView Browser Source** | Android System WebView GPU capture with URL/local HTML loading, custom CSS, touch interaction, zero-copy transparent backgrounds, and OBS mixer audio routing. |
| **USB & Camera Sources** | USB 3.0 UVC capture cards/webcams (`YUYV`, `NV12`, `RGB24`, `BGR24`, `MJPEG` with auto-matched UAC audio and watchdog recovery), built-in Camera2 NDK, and Nikon Z-series PTP Live View. |
| **Media & Network Playback** | MediaCodec hardware decoding (H.264, HEVC, VP8, VP9, AV1) with software fallback; Document URI playback and `HTTP(S)`, `HLS`, `DASH`, `RTSP(S)`, `RTMP(S)`, `SRT`, `RIST`, `UDP`/`RTP`/`TCP` streams. |
| **Audio Routing** | Native AAudio input for built-in microphones, wired headsets, and USB audio / capture cards. |
| **Shared Storage Recording** | MediaStore / SAF bridge for direct Hybrid MP4/MOV recording to public folders such as `Download/obs`. |
| **Adaptive Landscape UI** | Qt `6.12` responsive layout (`< 952×552` logical px compact phone mode vs. tablet workspace) with split-screen, floating-window, and IME adaptations. |

## Requirements

* **Architecture & OS**: `arm64-v8a`, Android 10+ (API 29+), Target SDK 35 (Compile SDK 36).
* **Prerequisites**: Android System WebView (required for the Browser Source). Available MediaCodec profiles and 10-bit HDR render targets depend on device hardware.

## Quick Start

1. **Install**: Download an APK from [Releases](https://github.com/Keee7/OBS-for-Android/releases) and run `adb install -r path/to/obs-android.apk`, or compile from source using [BUILDING.md](BUILDING.md).
2. **Baseline Setup**:
   * **Video**: `1920×1080` @ `60 FPS`, `SDR BT.709` (for 10-bit HDR, switch to `Rec. 2100 (PQ)` or `Rec. 2100 (HLG)` in **Settings → Advanced → Video** with `P010` + `MediaCodec HEVC Main10`, or `I010` for `x265` / `libaom`).
   * **Output**: `Android MediaCodec H.264` (`CBR`) for streaming; `Hybrid MP4` targeting `/storage/emulated/0/Download/obs` for recording.
3. **Add Sources**:
   * Add **Screen Capture (MediaProjection)** (grant system consent; use *Request screen sharing permission* in properties to re-authorize), **UVC Camera (Android USB)**, **Built-in Camera (Camera2)**, **Nikon Webcam**, or **Android Browser**.
   * For network streams, add **Media Source**, clear **Local File**, and enter an `HTTP(S)`, `HLS`, `DASH`, `RTSP`, `RTMP`, `SRT`, or `RIST` URL (pass FFmpeg flags such as `rtsp_transport=tcp` in **FFmpeg Options** when needed).
4. **Verify Hardware Encoding** *(optional)*:
   ```sh
   adb logcat -v threadtime -d -s 'OBS-MediaCodec:*'
   ```
   Confirm `event=started hardware=true software=false input=Surface`, `event=first-packet headers=valid keyframe=1`, and rising counters in `event=progress`.

## Known Limitations

* **HDR / 10-bit**: Requires GPU support for 16-bit normalized textures and floating-point render targets. Preview output and Android screen capture are composited as SDR at the configured SDR white level; P010 USB input requires compatible capture hardware.
* **Browser Source**: Multi-touch gestures are not yet supported; capturing cross-origin page audio requires server CORS headers.
* **Streaming Services**: Twitch and YouTube OAuth logins are disabled; configure RTMP server URLs and stream keys manually via the **Custom** service option.

## Documentation & License

* [BUILDING.md](BUILDING.md) — GitHub Actions, Linux/WSL, and Windows build instructions
* [IMPLEMENTATION_ANDROID.md](IMPLEMENTATION_ANDROID.md) — Architecture of the EGL/MediaCodec, MediaProjection, UVC, SAF storage, and Qt UI layers
* [CONTRIBUTING.md](CONTRIBUTING.md) — Issue reporting and PR guidelines
* [NOTICE.md](NOTICE.md) — Third-party licenses and trademark notices

Licensed under **GPL-2.0-or-later** ([LICENSE](LICENSE)). This is an independent community port and is not affiliated with the OBS Project.
