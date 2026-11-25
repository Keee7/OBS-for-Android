# Stream Stuttering & Dropped Frames

Stuttering, audio desync, or viewer buffering usually stems from software encoding fallback, USB bandwidth saturation, or unstable mobile uplink.

## 1. Encoder & Render Bottlenecks (Status Bar FPS Below Target)

1. **Software Encoder Accidentally Selected**: Check **Settings → Output → Video Encoder** and confirm **`Android MediaCodec H.264`** (or HEVC) is active rather than `Software (x264)` / `x265` / `libaom`.
2. **Full Color Range Triggered Software Fallback**: Verify **Settings → Advanced → Video → Color Range** is **`Limited`**, not `Full`.
3. **Excessive Capture or Compositor Load**:
   * Keep Screen Capture on **`1080p cap`**;
   * If thermal throttling starts, lower **Settings → Video → Output (Scaled) Resolution** to `1280x720` or reduce FPS from `60` to `30`.
4. **High-FPS Browser Overlays**: Lower chat overlay **Max FPS** to `30` or `20`.

## 2. Network Uplink Buffering (Status FPS Normal, Viewers Buffer)

* **Lower Stream Bitrate**: For outdoor 4G/5G streams, keep video bitrate around `2500–3500 Kbps` so bursts don't exceed sustained upload speed.
* **Enable Dynamic Bitrate**: Under **Settings → Advanced → Network**, check **Dynamically change bitrate to manage congestion** so OBS scales bitrate smoothly during signal dips.

## 3. UVC Capture Card Drops or Stalls

* If you see **USB bandwidth** warnings or choppy capture, switch to a verified **USB 3.0** cable or change the capture card format to **`MJPEG`**.
* Never force high-FPS uncompressed `YUYV` through a USB 2.0 cable or hub—saturating the bus triggers continuous watchdog reconnects.
