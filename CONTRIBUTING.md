# Contributing / 参与贡献

Issues and pull requests are welcome in English or Chinese.

## Reporting a problem

Include the app version, device model/SoC, Android version, actual codec component name, source type, resolution/frame rate, reproduction steps and whether the result differs between preview and recording. For storage issues, include the destination/provider and recording container; for media playback, include codec, bit depth and frame dimensions.

Remove stream keys, account tokens, private paths, IP addresses and personal media from logs/screenshots before posting. Do not upload an entire OBS configuration directory.

问题反馈请提供应用版本、设备/芯片、Android 版本、实际编解码组件、来源类型、分辨率/帧率及复现步骤，并区分预览与录像是否同时异常。上传前删除密钥、令牌、私人路径和个人媒体内容。

## Changes

- Start from `main`, keep changes focused and explain the resulting behavior.
- Follow existing OBS formatting and preserve upstream notices. New port code follows GPL-2.0-or-later.
- Consult [BUILDING.md](BUILDING.md). Run checks relevant to the change and record the device/test scope; explicitly distinguish host-only checks from device validation.
- Hardware codec work should include the actual component and output evidence. A configuration/initialization log alone does not prove sustained encoding/decoding.
- Preserve GPU retry without automatic CPU fallback for screen capture. Keep manual CPU selection available.
- Do not commit generated dependencies, native staging libraries, APKs, build caches, device captures or private signing keys.
- Include reproducible FFmpeg patches when changing the decoder dependency; an untracked local library replacement cannot be reproduced by other contributors.

历史实现记录在 [IMPLEMENTATION_ANDROID.md](IMPLEMENTATION_ANDROID.md)；当前默认值以 README 和对应源码为准。真实平台推流、其他芯片、HEVC、浏览器兼容性、透明采集和长期温控测试都欢迎贡献可复现结果。
