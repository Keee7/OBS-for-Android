# Android Version Differences

OBS for Android supports **Android 10 (API 29)** and above (Compiled API 36 / Target API 35). Key differences across Android releases:

| Android Version | Notable Differences & Capabilities |
| --- | --- |
| **Android 10 – 12**<br/>*(API 29 – 32)* | • Full core feature set (MediaCodec HW encode, MediaProjection whole-screen & internal audio, Camera2, UVC, WebView, Hybrid MP4).<br/>• Screen sharing captures the **entire display** only (no single-app isolation).<br/>• Hardware **HEVC Main10 (HDR)** is unavailable; enabling HDR falls back to `I010` + `x265` software encoding. |
| **Android 13**<br/>*(API 33)* | • Adds runtime **`POST_NOTIFICATIONS`** permission—must be allowed so `ObsForegroundService` stays alive in background.<br/>• Unlocks system **MediaCodec HEVC Main10 (10-bit HDR)** hardware encoding (where supported by SoC drivers). |
| **Android 14 – 15**<br/>*(API 34 – 35)* | • Adds **Single-App Screen Sharing** plus `onCapturedContentResize` handling so capture auto-adapts across rotations and split-screen resizing.<br/>• Enforces typed foreground service permissions (`specialUse \| mediaProjection \| camera \| microphone`, preconfigured in manifest) and preserves a single persistent `VirtualDisplay` across rotations without re-prompting. |
| **Android 16+**<br/>*(API 36+)* | • **AAudio MMAP Path Control**: Enables optional low-latency `MMAP` mode inside AAudio source properties (safely ignored on earlier OS versions). |

> [!NOTE]
> Across all Android 10+ Scoped Storage releases, `ObsStorageHost` (MediaStore / SAF) writes consistently into `Movies/OBS` or your chosen SAF folder without version-specific path breakage.
