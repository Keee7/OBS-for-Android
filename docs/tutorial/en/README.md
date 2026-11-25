# OBS for Android Documentation

Welcome to **OBS for Android**! This community port brings OBS Studio v33 to Android phones and tablets, featuring MediaCodec zero-copy hardware acceleration, multi-source audio/video capture, and an adaptive landscape workspace.

---

## Table of Contents

### Quick Start
* [Download and Install](quick-start/download-and-install.md) — APK sideloading, ADB install, in-app update checks, and source build links
* [System & Device Requirements](quick-start/system-requirements.md) — Supported architecture/OS versions, permissions, and key differences from desktop OBS
* [Interface Overview & Touch Controls](quick-start/interface-overview.md) — Tablet vs. phone layouts, the `☰` menu, and touch tips
* [Your First Stream](quick-start/first-stream.md) — Go live or test a recording in 5 minutes across 4 steps

### Scenes & Sources
* [Scene Management](scenes-and-sources/scene-management.md) — Source layering, Studio Mode, transitions, and filters
* [Camera & Screen Capture](scenes-and-sources/camera-and-screen.md) — MediaProjection single-app sharing, Camera2 (Flip Camera), and Nikon Z Series
* [USB/UVC Capture Cards](scenes-and-sources/usb-uvc-capture.md) — USB permission, MJPEG/YUV modes, low latency, and auto-reconnect
* [Android Browser & Chat Overlays](scenes-and-sources/browser-overlay.md) — Transparent WebView overlays, custom CSS, audio control, and touch interaction
* [Network Media Sources](scenes-and-sources/network-media-source.md) — Local playback and SRT / RIST / RTSP / HLS stream ingestion

### Streaming & Recording
* [RTMP / SRT Streaming & Protocols](streaming-and-recording/rtmp-and-srt.md) — Custom RTMP/RTMPS streaming and ingest vs. output protocol scope
* [Video Encoders](streaming-and-recording/video-encoders.md) — MediaCodec H.264/HEVC/AV1/VP9 vs. software encoder selection and bitrates
* [Audio Settings](streaming-and-recording/audio-settings.md) — AAudio input presets, exclusive/low-latency modes, and RNNoise filters
* [Local Recording](streaming-and-recording/local-recording.md) — Hybrid MP4/MOV, SAF shared-folder recording, and gallery access
* [Background Keep-Alive Guide](streaming-and-recording/background-keepalive.md) — Foreground service behavior and battery-optimization whitelist steps

### Advanced
* [HDR & Color Spaces](advanced/hdr-and-colorspace.md) — Rec. 2100 PQ/HLG, P010 hardware vs. I010 software encoding, and SDR white level
* [Hardware Encoding](advanced/hardware-encoding.md) — Zero-copy EGL Surface architecture, CBR/VBR/CQ, and ADB verification
* [Performance & Power Optimization](advanced/performance-and-power.md) — Partial dirty-region rendering, resolution/FPS trade-offs, and cooling/PD power
* [NDI® LAN I/O & Scene Recipes](advanced/ndi-and-recipes.md) — Importing `libndi.so`, wireless NDI® workflows, and 3 ready-to-use setups

### Compatibility
* [SoC & Encoder Support](compatibility/soc-and-encoders.md) — How mobile chips differ across H.264, HEVC Main10, AV1, and Full color range
* [Capture Card Compatibility](compatibility/capture-card-compatibility.md) — USB 3.0 cable requirements, format picks by card tier, and UAC audio
* [Android Version Differences](compatibility/android-versions.md) — Feature and permission differences across Android 10–12, 13+, 14+, and API 36+

### Troubleshooting
* [Black Screen Capture](troubleshooting/capture-black-screen.md) — Fixing black frames in Screen Capture, Camera2, UVC, and Browser sources
* [Stream Stuttering & Dropped Frames](troubleshooting/stream-stuttering.md) — Fixing software fallback, cellular jitter, and USB bandwidth bottlenecks
* [Encoder Errors](troubleshooting/encoder-errors.md) — Quick lookup for fallback banners, 5-second timeouts, and storage limits
* [Logs & Bug Reporting](troubleshooting/logs-and-feedback.md) — In-app logs, `adb logcat`, and Plugin Manager diagnostics

### Project
* [Changelog](project/changelog.md) — Summary of v0.2.5, v0.2.6, v0.2.7, and v0.2.8 milestones
* [Contributing](project/contributing.md) — Bug report checklist, PR rules, and log sanitization
* [Licenses & Notices](project/license.md) — GPL-2.0-or-later, third-party dependencies, and trademark notices
