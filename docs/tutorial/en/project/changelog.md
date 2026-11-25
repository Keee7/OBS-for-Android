# Changelog

Highlights compiled from major repository releases and commit milestones (full history on [GitHub Commits](https://github.com/Keee7/OBS-for-Android/commits/main) and [Releases](https://github.com/Keee7/OBS-for-Android/releases)).

## v0.2.7 (Current Baseline)

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
