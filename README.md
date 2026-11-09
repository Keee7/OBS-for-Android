# OBS for Android

**English** | [简体中文](README_CN.md)

An Android port of OBS Studio with scenes, sources, recording and streaming. This fork adds **MediaCodec H.264/HEVC hardware encoding, hardware-decoded media playback, a WebView browser source, GPU screen capture and Android shared-storage recording**.

Based on [cunminF/OBS_for_Android](https://github.com/cunminF/OBS_for_Android), which ports [OBS Studio 32.2.1](https://github.com/obsproject/obs-studio) to Android. This is a community project, not an official OBS Project Android release. Original authors, license notices and commit history are preserved.

**Current test version:** `0.1.22-cadence-diagnostics` / versionCode `23`. Package `com.antutu.ABenchMark`; **arm64-v8a, Android 10+ (API 29), target API 35**. This package installs alongside the previous `com.obsproject.studio` build and uses a separate Android private-data directory; existing OBS settings and service credentials are not migrated. The Java and JNI class namespace remains `com.obsproject.studio`. The Qt UI primarily targets landscape tablets. Local builds default to a private, external release signing configuration. Public debug signing requires an explicit option. See [recovery fixes and validation](scripts/RECOVERY_CADENCE_TEST.md).

## Features and validation

“Implemented” means the source and test APK build successfully; device evidence is listed separately.

| Feature | Implementation and evidence |
| --- | --- |
| H.264 hardware encoding | Named hardware MediaCodec component and Surface/GPU input. Xring O1 `c2.xring.avc.encoder` tested at 1920×1080/60 fps over local RTMP. |
| HEVC / H.265 hardware encoding | Main-compatible 8-bit SDR, VPS/SPS/PPS handling, capability checks and Surface input. Host tests/build checks passed; device HEVC recording needs validation. |
| Hardware encoding quality | Advanced output exposes supported CBR/VBR/CQ and Automatic/Low/Medium/High settings mapped to component complexity/quality ranges. MediaCodec does not use x264 presets. Device quality comparisons remain pending. |
| GPU screen capture | MediaProjection → SurfaceTexture/OES → OBS composition. New sources default to GPU; failures retry with backoff, without automatic CPU fallback. Manual CPU mode remains available. GPU capture was confirmed working on the test tablet. |
| Single-app capture and Alpha | Android 14+ captured-content size callbacks; compatible, straight-Alpha and premultiplied-Alpha modes. Host composition tests passed. VTube Studio → Android projection → OBS transparency is **not yet verified on-device**. |
| Browser source | System WebView via GPU texture: URL, size, FPS limit, CSS, refresh, visibility, basic pointer interaction and an optional transparent background. The transparent path drives the virtual display through an ImageReader output surface (keeps the compositor on its GPU path, so alpha survives) and samples the AHardwareBuffer through an EGLImage, with no CPU copy; verified on Xiaomi Pad 25053RP5CC. No CEF plugin support or separate web-audio track. |
| Audio input | AAudio capture with devices from Android AudioManager (built-in mic, wired headset, USB audio/capture-card audio), selectable in the source properties. UVC card audio uses the system UAC path and requires the ROM to expose that USB input. |
| Media playback and hardware decoding | Android document URI input and named MediaCodec hardware decoders through patched FFmpeg. A 4K H.264 cropping problem was reproduced and fixed on Xring O1, including after restarting the app. Other formats need individual validation. |
| Shared-folder recording | MediaStore / SAF for local seekable destinations such as `Download/obs`; fixes failed path-space queries being reported as insufficient space. Use Hybrid MP4/MOV; folder/provider combinations require device testing. |
| Floating-window preview | Unified Qt/Android window coordinates. Entering, moving, resizing and leaving floating-window mode checked on the test tablet. |
| RTMP streaming | Reconnect state reset, CBR/VBR dynamic bitrate and packet cleanup on send failures. Local device tests covered normal streaming, forced disconnection and bandwidth limitation. |
| 1080p60 YUV capture-card path | Independent USB pump, bounded buffer handoff, standard raw payload assembly, reduced copying and a YUYV/NV12 low-latency option. Host tests/build checks; capture-card device throughput not yet validated. [Test guide](scripts/UVC_1080P60_TEST.md). |
| UVC recovery | Exclusive USB loans, retained detached descriptors, valid-frame watchdog recovery and rate-limited USB software reset, with classified transport diagnostics. A software reset does not guarantee a physical power cycle. |
| Camera and USB sources | Retains upstream Camera2, system audio, Nikon PTP and UVC. Published upstream device results have not all been repeated in this fork. |

Hardware encoding reports initialization failures instead of silently choosing a software encoder. Media-source decoding can fall back to software **during initialization**; this is independent of the screen-capture retry policy.

## Device evidence and performance scope

Recent tests targeted an **Xring O1 tablet running Android 16**, with 1080p60 output:

- Local RTMP: 88.600 seconds, 5,317 video frames, measured 60.000 fps and approximately 6.028 Mbps. The received file decoded without errors; one rendering lag and one encoding lag were recorded.
- Forced receiver disconnect: reconnected in approximately 1.267 seconds; both received segments decoded successfully.
- A local 2 Mbps limit reduced measured encoder output to approximately 1.711 Mbps over five seconds; after removing the limit it recovered to approximately 5.836 Mbps toward a 6 Mbps target.
- Input was an existing 4K H.264 media file. A 60 fps output does not establish 60 distinct input-motion frames.

These are USB-forwarded local RTMP results, not a guarantee for live platforms, real Wi-Fi loss, RTMPS, long thermal runs or every Android chipset. HEVC, browser performance and transparent capture require further device tests. See [the device record](scripts/DEVICE_VALIDATION_WINDOW_STREAM.md).

The image below is inherited from the original Android port and illustrates its UI; it is not evidence of this fork's hardware-encoder tests.

![Original Android port's screen-capture preview](screenshot.png)

## Getting started

1. Build a test APK using [BUILDING.md](BUILDING.md). APKs and dependency binaries are excluded from Git. Published downloads, when available, belong in [Releases](https://github.com/Keee7/OBS-for-Android/releases).
2. Install with `adb install -r <test.apk>`. Back up configuration before changing signing keys or uninstalling an existing build.
3. Start with 1920×1080, 60 fps, SDR BT.709 and Android MediaCodec H.264. In Advanced output start with CBR and Automatic quality, then check output statistics.
4. Add a screen source and approve Android's sharing dialog. New sources default to GPU; existing saved CPU choices remain respected. Reopen source properties to inspect frame counters.
5. For shared storage, choose Hybrid MP4, disable file splitting and use `/storage/emulated/0/Download/obs` or authorize a local subfolder through the directory picker.
6. For media, select the video with Browse and enable “Use hardware decoding when available.” Inspect the logged fallback reason if initialization fails.

For transparent composition, put a color/image source beneath the screen source, select the target app in Android's sharing dialog and try **Preserve Alpha (premultiplied input)**. This preserves Alpha delivered by Android; it cannot reconstruct transparency after a background has been composited. Ordinary H.264/HEVC streaming sends the final scene, not Alpha-bearing video. See [the Alpha guide](scripts/TRANSPARENT_CAPTURE_TEST.md).

## Verify hardware encoding

Record moving content for 30–60 seconds and collect the same encoder session:

```sh
adb logcat -v threadtime -d -s 'OBS-MediaCodec:*' > mediacodec-log.txt
python scripts/verify-mediacodec-log.py mediacodec-log.txt --codec h264
```

For HEVC, use `--codec hevc`. Require all of these:

- `event=started`, an actual component name and `hardware=true software=false input=Surface`.
- `event=first-packet headers=valid keyframe=1`.
- Increasing `submitted`, `output`, `delivered` and `bytes` counters.
- A clean `event=summary failed=0`, without an error for that session.

A selected encoder name or low CPU usage alone is insufficient. Inspect the recording with FFmpeg/ffprobe and check OBS rendering/encoding-lag statistics. See [the hardware encoder guide](scripts/HEVC_HARDWARE_TEST.md).

## Limitations

- Codec capabilities, throughput and quality depend on the device. No HDR/Main10 or universal sustained-1080p60 guarantee.
- GPU screen capture retries without automatic CPU fallback. Revoked sharing consent must be renewed.
- Media hardware decoding produces CPU-readable frames; it is not end-to-end GPU zero-copy.
- Shared-document recording requires a local seekable provider. File splitting, replay buffer, custom FFmpeg output and automatic remux are not supported for that destination path. Real low-space conditions still stop recording.
- Browser source has no separate audio track, local HTML picker or full keyboard/wheel/multitouch integration. Browser Alpha needs device validation; plain HTTP follows the app's network policy.
- No implemented NDI/Spout2 input. VTube Studio transparency remains experimental.
- Scripting and built-in Twitch/YouTube/Restream integrations remain disabled. Configure a supported server URL/stream key manually.

## Source and documentation

```text
obs-studio/          OBS source, Android platform/frontend and plugins
  plugins/android-mediacodec/   H.264/HEVC hardware encoders
  plugins/android-screen/       GPU/CPU screen capture and Alpha handling
  plugins/android-browser/      WebView browser source
android-shell/       Early integration shell and public development debug key
deps-android/        Dependency versions, downloads and WSL/Linux cross-build scripts
scripts/             Windows build helpers, FFmpeg patches and test guides
```

[Build](BUILDING.md) · [Contribute](CONTRIBUTING.md) · [Latest test-package notes](scripts/DEVICE_TEST_QUICKSTART.md) · [Media/quality](scripts/MEDIA_QUALITY_TEST.md) · [Storage](scripts/ANDROID_STORAGE_TEST.md) · [Screen capture](scripts/ANDROID_PROJECTION_TEST.md) · [1080p60 acceptance](scripts/android-1080p60-acceptance.md)

[Implementation notes](IMPLEMENTATION_ANDROID.md) and some test guides contain historical version-specific findings. Current defaults are documented above and in the latest test-package notes. Report device/app/Android versions, component names, reproduction steps and sanitized logs in [Issues](https://github.com/Keee7/OBS-for-Android/issues). Remove stream keys and personal paths.

## License and attribution

This port follows upstream OBS Studio's **GPL-2.0-or-later** licensing. See [LICENSE](LICENSE), [OBS COPYING](obs-studio/COPYING), [OBS AUTHORS](obs-studio/AUTHORS) and [NOTICE.md](NOTICE.md). Dependencies retain their own licenses. The debug keystore is public development material, not a private release signing key.

Thanks to [cunminF](https://github.com/cunminF/OBS_for_Android), the [OBS Project](https://github.com/obsproject/obs-studio) and dependency authors. OBS names and icons remain attributable to their respective owners.
