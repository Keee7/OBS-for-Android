# SoC 与编码器支持

OBS for Android 在运行期动态查询当前设备 `MediaCodecList` 与 OpenGL ES GPU 能力，因此不同 SoC 芯片平台可用的硬件编码器与高阶特性会有所差异。

## 1. 硬件编码器支持矩阵（通用规律）

| 编码格式 | 硬件支持情况 | 常见适用芯片与系统前提 |
| --- | --- | --- |
| **H.264 (`video/avc`)** | 几乎全平台支持 | Android 10+ 全系 Snapdragon / Dimensity / Exynos / Kirin 均原生支持 |
| **HEVC (`video/hevc`)** | 绝大多数支持 | 主流中高端 SoC 均支持 8-bit HEVC 硬编 |
| **HEVC Main10 (10-bit HDR)** | 需 **Android 13+** 且硬件支持 | 需芯片支持 10-bit `P010` HEVC Profile 2，并在「高级 → 视频」开启 `Rec. 2100` 后开放 |
| **AV1 (`video/av01`)** | 仅新款旗舰芯片支持 | 需设备内置硬件 AV1 编码器（如部分新一代旗舰 SoC），不支持的设备列表中不会显示 |
| **VP9 (`video/x-vnd.on2.vp9`)** | 部分芯片支持 | 仅用于本地 Hybrid MP4 录制（如部分 Pixel / 高通平台），不支持的芯片自动隐藏 |

## 2. 色彩范围与复杂度的硬件差异

* **Limited vs Full 色彩范围**：部分手机的 MediaCodec H.264/HEVC 硬件编码器在驱动层仅接受 `Limited（常规，16–235）` 色彩范围。如果用户在 **高级 → 视频** 强制选为 `Full（完整，0–255）`，OBS 会在启动前做能力校验，不支持时自动弹出提示并回退到软件编码。
* **Hardware quality（复杂度/质量档位）**：`Automatic` 适用于所有芯片；只有部分支持动态调节质量或复杂度区间的芯片，显式选择 `Low / Medium / High` 才能正常启动。
* **Surface 输入 vs ByteBuffer YUV**：主流芯片均支持零拷贝 EGL Surface 输入；极个别定制 ROM 若 Surface 初始化失败，OBS 会自动衔接 ByteBuffer YUV（`NV12` / `I420`）路径保证可用。
