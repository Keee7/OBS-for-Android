# Building OBS for Android / 构建说明

The current test APK is `0.1.22-cadence-diagnostics` (23), package `com.antutu.ABenchMark`, arm64-v8a, minSdk 29 / targetSdk 35. This is a separate Android package from the previous `com.obsproject.studio` build, so it installs alongside it with independent private data; no OBS settings or service credentials are migrated. Java/JNI classes retain the `com.obsproject.studio` namespace. Full native/Qt/Java/APK builds and strict artifact checks passed on Windows for this version. WSL/Linux is the original port's build route; the additional WSL script preparation changes below have host checks, not a new full Linux build.

当前 versionCode 23 测试包已完成 Windows 全量 configure、native/Qt/Java/APK 构建、release 签名与 APK 严格核验。下面分别说明 Windows 工具布局和原项目的 WSL 构建路径；工具链和依赖源码不随仓库提供，需要自行安装/下载。

## Clone

```sh
git clone https://github.com/Keee7/OBS-for-Android.git
cd OBS-for-Android
```

`obs-studio/` is included as source, not a Git submodule. Do not download an unrelated desktop OBS binary to replace it.

## Toolchain

| Component | Version used / requirement |
| --- | --- |
| Android ABI / native API | arm64-v8a / android-29 |
| Android SDK | Platform android-35; build-tools 36.0.0; platform-tools for ADB |
| NDK | r30; Windows test environment: 30.0.16248370 |
| Qt | 6.9.3 Android arm64 plus matching host Qt with `androiddeployqt` |
| Java | JDK 17; Windows script expects directory `jdk-17.0.12` |
| CMake / Ninja | CMake 3.31.6; Ninja |
| Gradle | Windows 8.14.3; original WSL layout 8.14.5 |
| Windows native dependencies | MSYS2 Bash, make, patch, tar, pkg-config and LLVM tools |
| Host verification | Python 3; FFmpeg/ffprobe for recording verification |

Dependency versions and upstream URLs are in [deps-android/versions.sh](deps-android/versions.sh). It includes branch-based x264/rnnoise entries, so a later fresh download is not a byte-reproducible copy of the tested dependency set. Retain source archives/revisions with a distributed binary.

## Windows

### Tool and dependency layout

[build-frontend-apk-windows.ps1](scripts/build-frontend-apk-windows.ps1) accepts `-ToolsRoot`; by default this is a `build-tools` directory next to the repository. Its expected layout is:

```text
build-tools/
  android-sdk/                  platforms/android-35, build-tools/36.0.0, platform-tools
  ndk/android-ndk-r30/           Complete Windows NDK, including source.properties
  Qt/6.9.3/android_arm64_v8a/
  Qt/6.9.3/msvc2022_64/          Host Qt, including bin/androiddeployqt.exe
  jdk-17.0.12/
  gradle/gradle-8.14.3/
  cmake/cmake-3.31.6-windows-x86_64/
  ninja/ninja.exe
  android-deps/arm64-v8a/        Cross-compiled include/, lib/, CMake/pkg-config metadata
```

The build script does not install these tools or download a dependency prefix. That prefix must contain the dependencies required by the enabled plugins; see `deps-android/versions.sh` and the CMake finders. All binary libraries must target Android arm64, not Windows.

### Preparing dependencies

- [build-android-cmake-deps.py](scripts/build-android-cmake-deps.py) builds Jansson, libjpeg-turbo, FreeType, mbedTLS and curl. It expects the source directory layout listed in that script and copies SIMDe/uthash headers when present.
- [build-deps-windows.sh](scripts/build-deps-windows.sh) builds x264 and FFmpeg under MSYS2. It requires `WORK_ROOT` (absolute, no spaces), `SYSROOT` (complete NDK sysroot), `SOURCES_ROOT` (containing `x264/` and `ffmpeg-7.1.3/`) and `OUTPUT_PREFIX`. `LLVM_BIN` can override the LLVM directory.
- USB/audio-filter dependencies also need to be present in the prefix; the Python helper is not an installer for the entire dependency set. The WSL route below covers the original dependency groups.

Example for the Python helper (adapt paths to your installation):

```powershell
python scripts/build-android-cmake-deps.py --tools D:/build-tools --ndk D:/build-tools/ndk/android-ndk-r30
```

Both FFmpeg build routes apply these repository patches before compilation:

- `scripts/android-native-deps/ffmpeg-7.1.3-mediacodec-codec-name.patch`
- `scripts/android-native-deps/ffmpeg-7.1.3-mediacodec-crop.patch`

An older, unpatched `libavcodec.a` will not contain the named decoder selection and output-format/cropping fixes. Rebuild FFmpeg and the final APK after changing these patches.

### Build and package

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-frontend-apk-windows.ps1 -Action all -ToolsRoot D:/build-tools
```

For incremental builds, run **both** steps:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-frontend-apk-windows.ps1 -Action build -ToolsRoot D:/build-tools
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-frontend-apk-windows.ps1 -Action package -ToolsRoot D:/build-tools
```

Packaging first builds current sources, stages the latest plugin libraries, deploys Java/resources, aligns the APK and signs/verifies it with the external release key. Set `OBS_ANDROID_SIGNING_CONFIG` or pass `-SigningConfigFile`; the default is `Documents/release key.txt` outside the repository. Missing credentials fail signing. Use `-DebugSigning` only when explicitly choosing the public development key. The signed APK appears under `releases/`; these files are ignored by Git. A native build alone is insufficient to guarantee the latest plugins are in the installable APK.

### Inspect the APK

```powershell
python scripts/verify-android-apk.py releases/<test.apk> --tools-root D:/build-tools --native-dir D:/build-tools/build-obs-arm64
```

This Windows verifier checks the source manifest version, required Java classes/callback, Alpha options, native architecture/symbols and library equality with the build tree. It does not perform a device/runtime test. The packaging script separately verifies the signature and 16 KB ZIP alignment.

## WSL / Linux

Install the Linux NDK, Qt Android plus host Qt, SDK, JDK 17, CMake, Ninja, Gradle and host build tools. Set environment variables for your installation:

```sh
export ANDROID_SDK_ROOT="$HOME/Android/Sdk"
export NDK_ROOT="$ANDROID_SDK_ROOT/ndk/<installed-r30-version>"
export QT_DIR="$HOME/Qt/6.9.3"
export QT_HOST_PATH="$QT_DIR/gcc_64"
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
export GRADLE_BIN="$HOME/toolchain/gradle-8.14.5/bin"
```

The WSL frontend script also reads `~/.obs-android-env.sh` when present. Repository location is derived from the script path; `OBS_SOURCE_ROOT` may override it.

Download and build the dependency groups, then build the APK:

```sh
bash deps-android/fetch.sh
NDK="$NDK_ROOT" bash deps-android/build.sh arm64-v8a
bash scripts/build-frontend-apk-wsl.sh
```

`fetch.sh` uses curl's normal network configuration by default. If needed, set `OBS_PROXY` for GitHub downloads; no machine-specific proxy is required by the repository. Dependency output is `deps-android/prebuilt/arm64-v8a/`. Existing cached FFmpeg builds need rebuilding to apply the new patches:

```sh
NDK="$NDK_ROOT" bash deps-android/build.sh arm64-v8a ffmpeg --clean
```

## Regression and device checks

Portable verifier unit tests:

```sh
python -m unittest discover -s scripts -p 'test_verify_*.py' -v
```

Windows checks with the expected adjacent tool layout:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-android-screen.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-android-uvc.ps1
python scripts/test-android-display-rect.py
python scripts/test-android-interop-shaders.py --egl-dir <directory-containing-ANGLE-DLLs>
```

The screen test accepts `-ToolsRoot` and `-Clang`. Other host harnesses may need compiler/tool paths adjusted for your environment. The ANGLE test exercises actual host GLES shaders and equivalent Alpha blending, not the Android compositor or Xring driver.

For device checks, use [the latest test-package notes](scripts/DEVICE_TEST_QUICKSTART.md), [hardware-encoder checks](scripts/HEVC_HARDWARE_TEST.md), [transparent capture](scripts/TRANSPARENT_CAPTURE_TEST.md) and [1080p60 acceptance](scripts/android-1080p60-acceptance.md). Do not treat a successful build or a host test as a substitute for on-device results.

## Signing and distribution

The bundled `android-shell/debug.keystore` is the upstream project's **public development key** (alias `androiddebugkey`, password `android`). Current scripts produce debuggable test builds. For your own release process, keep a separate private signing key outside Git, configure signing accordingly, and review the debug manifest setting. Changing the key prevents ordinary in-place upgrades from differently signed builds.

Preserve the source, dependency license notices and source/revision information corresponding to the binary you distribute. See [NOTICE.md](NOTICE.md).
