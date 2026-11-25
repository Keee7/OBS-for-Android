# 开源许可证与声明

本项目为 [OBS Studio](https://github.com/obsproject/obs-studio)（`v33` 基线）的社区维护 Android 移植版本，基于 [cunminF/OBS_for_Android](https://github.com/cunminF/OBS_for_Android) 构建。

## 1. 核心代码许可证

* **OBS Studio 及 Android 移植源码**：遵循 **GNU 通用公共许可证第 2 版或更高版本（GPL-2.0-or-later）**。完整协议文本见仓库 [LICENSE](https://github.com/Keee7/OBS-for-Android/blob/main/LICENSE)。

## 2. 主要第三方依赖许可

| 组件 | 许可证 | 用途 |
| --- | --- | --- |
| **Qt 6.12** | LGPLv3 / GPLv3 | 移动端跨平台图形界面与控件框架 |
| **FFmpeg** | GPL-3.0-or-later（Android 构建启用 `--enable-gpl --enable-version3`） | 媒体源解码、音频编码（AAC）与软件编码桥接 |
| **x264 / x265** | GPLv2 | 软件 H.264 / HEVC（8/10-bit）编码 |
| **libaom** | BSD-2-Clause + AOM Patent License 1.0 | 软件 AV1 编码与解码 |
| **SRT** | MPL-2.0 | SRT 媒体流拉取支持 |
| **librist** | BSD-2-Clause | RIST 媒体流拉取支持 |
| **libusb** | LGPL-2.1-or-later | 用户态 USB UVC 采集卡与 Nikon PTP 通信 |
| **libuvc 0.0.9** | BSD-3-Clause | 内嵌 Android 加固版 UVC 兼容采集后端 |
| **libxml2 / cJSON** | MIT | DASH 解析与配置/媒体辅助 |
| **mbedTLS / curl / FreeType / libjpeg-turbo / Jansson / RNNoise / SpeexDSP** | 见各自上游许可证 | TLS 加密推流、字体渲染、JPEG 解码、音频降噪等 |

完整第三方开源归属见仓库 [NOTICE.md](https://github.com/Keee7/OBS-for-Android/blob/main/NOTICE.md)。

## 3. 商标与独立性声明

* **OBS / OBS Studio**：“OBS” 及 “OBS Studio” 为 OBS Project 的商标或注册商标。本项目为社区独立维护的开源移植版本，与 OBS Project 无隶属、赞助或官方背书关系。
* **NDI®**：NDI® 是 Vizrt NDI AB 的注册商标。本应用与 Vizrt 无隶属或赞助关系；官方闭源 `libndi.so` 运行库由用户依据 BYOR 机制自行从官网获取，并受 NDI SDK 许可协议约束。
