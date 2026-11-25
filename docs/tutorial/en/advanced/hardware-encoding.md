# Hardware Encoding

OBS for Android features a mobile-tailored `android-mediacodec` plugin that delivers dramatic power savings and frame pacing stability over CPU software encoding.

## 1. Zero-Copy EGL Surface Architecture

* **Direct GPU Submission**: Every rendered frame from OBS's OpenGL ES compositor passes directly into MediaCodec's input **EGL Surface** with **no CPU readback or intermediate memory copy**.
* **Shared Context & Bidirectional GPU Fences**: Encoding runs on a dedicated shared `EGLContext` decoupled via bidirectional GPU Sync Fences—brief encoder stalls never freeze the UI preview.
* **Watchdog Overload Protection**: Tracks submission latency and packet progress; if the hardware encoder stalls with no output for 5 seconds or overflows the queue, OBS exits output safely instead of hanging silently.

## 2. Advanced Output Controls

Switch **Settings → Output → Output Mode** to **Advanced** to reveal full `Android MediaCodec` parameters:

* **Rate control**:
  * **`CBR`**: Constant Bitrate—best for live streaming to keep uplink pacing smooth.
  * **`VBR`**: Variable Bitrate—best for local recording to spend bits on motion and save space on calm scenes.
  * **`CQ (device quality range)`**: Constant Quality—targets hardware quality scales instead of a fixed kbps cap.
* **Hardware quality**: `Automatic`, `Low`, `Medium`, or `High`, mapped dynamically to driver-advertised complexity/quality ranges (`Automatic` is safest across heterogeneous SoC drivers).
* **Keyframe Interval**: Defaults to `2` seconds for standard live ingest compatibility.

## 3. Capability Probing & Automatic Fallback

Before starting an output, OBS probes `MediaCodecList` capabilities:
1. If an encoder rejects a requested `Full` color range, OBS automatically falls back to **ByteBuffer YUV (`NV12` / `I420`)** or software encoding and posts a clear notice.
2. When a `Rec. 2100` HDR scene runs on a device lacking hardware HEVC Main10, OBS falls back cleanly to **Software HEVC (x265)** with a status-bar notification.

## 4. Verifying Zero-Copy Encoding via ADB

With USB debugging active while streaming or recording, run:

```sh
adb logcat -v threadtime -d -s 'OBS-MediaCodec:*'
```

Look for `event=started hardware=true software=false input=Surface`, `event=first-packet headers=valid keyframe=1`, and steadily incrementing frames/bytes on `event=progress` lines.
