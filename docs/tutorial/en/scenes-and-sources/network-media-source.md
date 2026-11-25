# Network Media Sources

Using **Media Source** (backed by FFmpeg and Android MediaCodec hardware decoding), you can play local video clips or pull live network streams and IP cameras directly into your scenes.

## 1. Local File Playback

1. Add a **Media Source** and keep **Local File** checked.
2. Tap **Browse** to choose a video clip (supports Android SAF document URIs).
3. Configure **Loop**, **Restart playback when source becomes active**, or **Show nothing when playback ends** as needed.

## 2. Pulling Network Streams

**Uncheck "Local File"** to reveal the network stream fields:

* **Supported Protocols**: `HTTP(S)`, `HLS (.m3u8)`, `DASH (.mpd)`, `RTSP(S)`, `RTMP(S)`, `SRT (srt://)`, `RIST (rist://)`, `UDP`, `RTP`, `TCP`.
* **Input Examples**: Enter the full stream URI in **Input**, such as:
  * `srt://192.168.1.50:9000?mode=caller`
  * `rtsp://192.168.1.88:554/stream1`
  * `rtmp://example.com/live/streamkey`
* **FFmpeg Options**: When an RTSP camera or relay drops packets, smears macroblocks, or takes too long to buffer the first frame, pass space-separated `key=value` pairs under **FFmpeg Options**, for example:
  ```text
  rtsp_transport=tcp rtsp_flags=prefer_tcp
  ```

## 3. Hardware Decoding & Automatic Fallback

* **Use hardware decoding when available** is checked by default. Standard 8-bit SDR streams (`H.264`, `HEVC`, `VP8`, `VP9`, `AV1`) prefer **zero-copy MediaCodec Surface decoding** for minimal CPU load.
* OBS transparently switches to non-Surface decoding (`NV12`/`P010` ByteBuffer or software FFmpeg) without user intervention when:
  * Playing 10-bit or HDR (`PQ`/`HLG`) media;
  * Asynchronous video filters are attached to the source;
  * A custom color range, linear alpha, or VP8/VP9 alpha channel is enabled;
  * The device lacks hardware decode support for that codec.
