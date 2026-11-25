# Changelog

Highlights compiled from major repository releases and commit milestones (full history on [GitHub Commits](https://github.com/Keee7/OBS-for-Android/commits/main) and [Releases](https://github.com/Keee7/OBS-for-Android/releases)).

## v0.2.8 (Current Baseline)

* **UVC Dual-Backend Architecture & Large-Payload Bulk Support**:
  * Added a standard UVC Bulk Payload Transfer reassembler and ZLP boundary handling to the built-in native engine, enabling large-payload Bulk cameras (such as Sony A7M4 with `dwMaxPayloadTransferSize > 64 KiB`), and fixed stale USB paths in the Properties **Request USB Permission** button after replug/reset.
  * Bundled an Android-hardened `libuvc 0.0.9` backend (64 KiB chunked Bulk URBs, 8-packet ISO cap, real endpoint-type ISO/Bulk detection, bounded-timeout stop with async retirement) selectable via **Capture Backend** in UVC source properties.
  * Added `UYVY` uncompressed format support across both backends, `P010` capability filtering, and automatic `YUYV` black-screen fallback on the `libuvc` path.
* **Native NDI® LAN I/O (BYOR)**: Added the native `android-ndi` plugin for NDI® Input Source (mDNS `NsdManager` + manual IP discovery) and Main Program NDI® Output (dedicated bounded-queue sender thread), with a built-in official `libndi.so` importer validating ARM64 ABI, 16 KB page alignment, and required symbols.
* **Keep-Screen-On & UVC YUV Polish**: Added a keep-screen-on toggle for long broadcasts and refined UVC YUV mode-lock / lowest-latency option hints.
* **Bilingual Documentation Site**: Published the VitePress Chinese/English user manual with automated GitHub Pages deployment.

## v0.2.7

* **UVC Isochronous Capture**: Added UVC isochronous transfer support alongside hardened Bulk transfers and safer USB disconnect cleanup.
* **SRT Playback Stability**: Improved MediaCodec Surface decoding and GPU synchronization when pulling SRT streams in Media Source.
* **Color Range & Encoder Fallbacks**: Added `Limited / Full` probing per color space and bitrate mode, with graceful fallback to ByteBuffer YUV or software encoding (plus UI and status-bar notices) when Full Range or HDR Main10 isn't supported in hardware.
* **UI Rendering Efficiency**: Added Android RHI `BackingStore` partial dirty-region uploads and snapshot-based partial repaints for `VolumeMeter`, `MediaControls`, and `AbsoluteSlider`, lowering idle and active-mixer CPU overhead.
* **About Dialog & Visuals**: Updated About dialog info and `AUTHORS` packaging, refreshed bilingual READMEs, and added annotated WebP UI documentation previews.

## v0.2.6

* **In-App Update Checker**: Added automatic startup and manual GitHub Releases update checks.
* **Hardened UVC Audio Auto-Match**: Matched companion UAC audio endpoints strictly by USB path topology and `VID:PID`, moving audio retry work off the video capture thread.
* **Media Source Upgrades**: Enabled zero-copy MediaCodec Surface decode, improved P010 HDR, async filters, and end-frame retention, and redacted stream URLs / FFmpeg options from logs.
* **RTMP Disconnect Cleanup**: Fixed RTMP disconnect cleanup and buffered control packet handling.

## v0.2.5

* **Qt 6.12.0 LTS Upgrade**: Upgraded the Android UI stack to Qt 6.12.0 for faster settings dialogs and smoother combo-box scrolling.
* **MediaProjection / HDR / SAF Polish**: Added Android 14+ single-app capture resize handling and alpha compositing, 10-bit HDR (`Rec. 2100 PQ/HLG` + `P010`/`I010`), and SAF export/import for profiles and scene collections.
* **SRT Media Ingest**: Bundled MbedTLS, SRT, librist, and libxml2 into the Android dependency chain.
* **Phone Compact & Studio Mode**: Stabilized top/bottom Studio Mode on phones while keeping media playback controls accessible in the compact toolbar.
