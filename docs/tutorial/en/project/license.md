# Licenses & Notices

This project is a community-maintained Android port of [OBS Studio](https://github.com/obsproject/obs-studio) (`v33` baseline), built upon [cunminF/OBS_for_Android](https://github.com/cunminF/OBS_for_Android).

## 1. Core License

* **OBS Studio & Android Port Sources**: Licensed under the **GNU General Public License v2.0 or later (GPL-2.0-or-later)**. Full license text is in [LICENSE](https://github.com/Keee7/OBS-for-Android/blob/main/LICENSE).

## 2. Major Third-Party Components

| Component | License | Role |
| --- | --- | --- |
| **Qt 6.12** | LGPLv3 / GPLv3 | Mobile UI framework and widget layer |
| **FFmpeg** | GPL-3.0-or-later (`--enable-gpl --enable-version3`) | Media decoding, AAC audio encoding, software encoder bridge |
| **x264 / x265** | GPLv2 | Software H.264 / HEVC (8/10-bit) encoders |
| **libaom** | BSD-2-Clause + AOM Patent License 1.0 | Software AV1 encoding and decoding |
| **SRT** | MPL-2.0 | SRT media ingest |
| **librist** | BSD-2-Clause | RIST media ingest |
| **libusb** | LGPL-2.1-or-later | User-space USB UVC capture and Nikon PTP communication |
| **libuvc 0.0.9** | BSD-3-Clause | Bundled Android-hardened UVC compatibility capture backend |
| **libxml2 / cJSON** | MIT | DASH manifest parsing and configuration/media helpers |
| **mbedTLS / curl / FreeType / libjpeg-turbo / Jansson / RNNoise / SpeexDSP** | Upstream permissive licenses | RTMPS TLS transport, text rendering, JPEG decode, audio noise suppression |

See [NOTICE.md](https://github.com/Keee7/OBS-for-Android/blob/main/NOTICE.md) for full third-party attributions.

## 3. Trademark & Independence Notices

* **OBS / OBS Studio**: "OBS" and "OBS Studio" are trademarks of the OBS Project. This community port is independently maintained and is not affiliated with, endorsed by, or sponsored by the OBS Project.
* **NDI®**: NDI® is a registered trademark of Vizrt NDI AB. This project is not affiliated with or sponsored by Vizrt; the proprietary `libndi.so` runtime is acquired directly by the user under Vizrt's NDI SDK terms (BYOR).
