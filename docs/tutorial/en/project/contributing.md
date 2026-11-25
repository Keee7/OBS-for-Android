# Contributing

Contributions of code, documentation, and device compatibility reports are welcome in either English or Chinese!

## 1. Filing Bug Reports (Issues)

Please provide the following details so maintainers can reproduce and diagnose quickly:
1. **Device & OS**: Device model, SoC/chipset, Android version, and vendor ROM (HyperOS, ColorOS, OriginOS, One UI, etc.).
2. **App Build**: APK release tag and Git commit SHA (from About or the top of the OBS log).
3. **Encoder Details**: Actual MediaCodec hardware component reported in the OBS log.
4. **Scene Configuration**: Active source types, canvas/output resolutions, and target FPS.
5. **Reproduction Steps**: Clear steps and whether the issue occurs during preview, recording, or streaming.
6. **Sanitized Logs**: OBS log exports or `adb logcat` output—**always strip stream keys, account tokens, private LAN IPs, and personal paths before uploading**.

## 2. Pull Requests

* **Target Branch**: Open pull requests against `main`, keeping changes focused and modular.
* **License Compliance**: All code contributions must be licensed under **GPL-2.0-or-later** with upstream headers intact and no GPLv2-incompatible dependencies.
* **Build Verification**: Test APK builds following [BUILDING.md](https://github.com/Keee7/OBS-for-Android/blob/main/BUILDING.md).
* **Keep the Repo Clean**: Never commit build outputs (`.apk`, intermediate `.so` binaries, build caches) or signing keystores (`.jks` / `.keystore`).

See [CONTRIBUTING.md](https://github.com/Keee7/OBS-for-Android/blob/main/CONTRIBUTING.md) in the repository root for complete guidelines.
