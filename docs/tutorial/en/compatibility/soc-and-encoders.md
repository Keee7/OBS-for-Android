# SoC & Encoder Support

OBS for Android dynamically queries each device's `MediaCodecList` and OpenGL ES capabilities at runtime, so available encoders and advanced features vary by SoC family and driver.

## 1. Hardware Encoder Matrix (General Behavior)

| Codec | Typical Hardware Support | Common Chipset & OS Requirements |
| --- | --- | --- |
| **H.264 (`video/avc`)** | Near-universal | Natively supported across Snapdragon, Dimensity, Exynos, and Kirin devices on Android 10+ |
| **HEVC (`video/hevc`)** | Widely supported | 8-bit HEVC hardware encode available on nearly all modern mid-to-high-end SoCs |
| **HEVC Main10 (10-bit HDR)** | **Android 13+** & HW dependent | Requires SoC 10-bit `P010` HEVC Profile 2 support and `Rec. 2100` enabled in **Advanced → Video** |
| **AV1 (`video/av01`)** | Recent flagships only | Shown only when the SoC exposes a hardware AV1 encoder |
| **VP9 (`video/x-vnd.on2.vp9`)** | Select SoCs only | Intended for local Hybrid MP4 recording (e.g., select Pixel / Snapdragon devices); hidden otherwise |

## 2. Color Range, Complexity & Surface Quirks

* **Limited vs. Full Color Range**: Several mobile MediaCodec drivers only accept `Limited (16–235)` input. Selecting `Full (0–255)` in **Advanced → Video** triggers capability verification and, when unsupported, notifies you and falls back to software encoding.
* **Hardware Quality / Complexity**: `Automatic` works on every chip; explicit `Low / Medium / High` presets only succeed on drivers implementing dynamic complexity/quality ranges.
* **Surface vs. ByteBuffer YUV**: Mainstream SoCs use zero-copy EGL Surface input. On rare custom ROMs where Surface init fails, OBS automatically bridges via `NV12` / `I420` ByteBuffer mode.
