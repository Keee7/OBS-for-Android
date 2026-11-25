# 网络媒体源

通过 **媒体** 源（基于 FFmpeg 与 Android MediaCodec 硬解），你可以在手机 OBS 里播放本地素材视频，或直接拉取各类网络直播流与监控流。

## 1. 播放本地视频

1. 添加 **媒体** 源，保持勾选 **本地文件**。
2. 点击 **浏览** 选取本地视频文件（支持 Android SAF 文档 URI 路径）。
3. 可按需勾选「循环播放」、「激活时重新开始播放」或「播放结束时不显示任何内容」。

## 2. 拉取网络流

**取消勾选「本地文件」**，页面会切换到网络拉流输入框：

* **支持的协议**：`HTTP(S)`、`HLS (.m3u8)`、`DASH (.mpd)`、`RTSP(S)`、`RTMP(S)`、`SRT (srt://)`、`RIST (rist://)`、`UDP`、`RTP`、`TCP`。
* **输入格式**：在 **输入** 栏直接填写完整流地址，例如：
  * `srt://192.168.1.50:9000?mode=caller`
  * `rtsp://192.168.1.88:554/stream1`
  * `rtmp://example.com/live/streamkey`
* **FFmpeg 选项**：当遇到某些 RTSP 摄像头或服务器丢包、花屏、首帧慢时，可在 **FFmpeg 选项** 填写 `option=value`（多个参数用空格隔开），例如：
  ```text
  rtsp_transport=tcp rtsp_flags=prefer_tcp
  ```

## 3. 硬件解码与自动回退

* 默认勾选 **在可用时使用硬件解码**。对于常规 8-bit SDR（`H.264`、`HEVC`、`VP8`、`VP9`、`AV1`）流，OBS 会优先启用 **MediaCodec Surface 零拷贝硬解**，大幅降低 CPU 占用。
* 遇到以下情况时，系统会自动切换至非 Surface 路径（`NV12`/`P010` 或软解），无需人工干预：
  * 播放 10-bit / HDR（PQ/HLG）视频；
  * 为该媒体源挂了异步视频滤镜；
  * 手动指定了色彩范围或启用线性空间 Alpha、VP8/VP9 透明通道；
  * 当前设备硬解不支持该视频编码格式。
