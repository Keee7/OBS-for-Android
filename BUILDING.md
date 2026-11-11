# Building OBS for Android / 构建说明

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This guide describes how to build OBS for Android on Ubuntu / WSL, via GitHub Actions CI, and on native Windows.

* **Target Architecture**: `arm64-v8a`
* **API Level**: Minimum Android 10 (API 29), compile SDK 36 and target SDK 35
* **Default Package Name**: `com.obsproject.studio`

### Build Routes

| Route | Requirements | Output Artifact | Purpose |
| --- | --- | --- | --- |
| **GitHub Actions** | Hosted Ubuntu Runner | `obs-android-apk-<sha>` workflow artifact | Automated CI builds |
| **Ubuntu / WSL Automated Script** | Ubuntu 24.04 (or WSL2) | APK located in `build-fe-arm64-v8a-ci/` | One-click toolchain setup & dependency compilation |
| **WSL / Linux Existing Toolchain** | Configured NDK r30 & Qt 6.11.1 | `releases/OBS-Android-arm64-v8a-wsl-*.apk` | Local day-to-day incremental development |
| **Native Windows** | Windows 10/11 + MSYS2 + PowerShell | `releases/OBS-Android-arm64-obs-*.apk` | Native Windows cross-compilation & packaging |

### Toolchain Requirements

* **Android SDK**: Build-Tools `36.0.0`, Platform `android-36`, Platform-Tools (ADB)
* **Android NDK**: `r30` (30.0.16248370)
* **Qt**: `6.11.1` (Android arm64 package + matching host Qt with `androiddeployqt`)
* **Java**: JDK 17
* **Build System**: CMake 3.31+ and Ninja
* **Gradle**: 9.3.1
* **Host Tools**: Python 3; MSYS2 (for compiling specific native dependencies on Windows)

Dependency versions are tracked in [deps-android/versions.sh](deps-android/versions.sh).

### Ubuntu / WSL Automated Build

Run the automated bootstrap script on Ubuntu 24.04 or WSL2:

```sh
sudo apt-get update
sudo apt-get install -y 7zip ninja-build build-essential curl unzip zip ca-certificates \
  patch pkg-config dpkg-dev autoconf automake libtool nasm yasm python3-venv openjdk-17-jdk \
  libgl1-mesa-dev libegl1-mesa-dev libxkbcommon-dev

python3 -m venv "$HOME/.venvs/obs-android"
source "$HOME/.venvs/obs-android/bin/activate"
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64

bash scripts/ci-build-android.sh
cat build-fe-arm64-v8a-ci/.final-apk-path
```

<a name="existing-toolchain"></a>
### WSL / Linux with an existing toolchain

Run the commands from the repository root. Point the variables at your installed Linux kits.

```sh
export ANDROID_SDK_ROOT="$HOME/Android/Sdk"
export NDK_ROOT="$ANDROID_SDK_ROOT/ndk/30.0.16248370"
export QT_DIR="$HOME/Qt/6.11.1"
export QT_HOST_PATH="$QT_DIR/gcc_64"
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
export GRADLE_BIN="$HOME/toolchain/gradle-9.3.1/bin"

bash deps-android/fetch.sh
NDK="$NDK_ROOT" bash deps-android/build.sh arm64-v8a
bash scripts/build-frontend-apk-wsl.sh
```

CMake, Ninja and the host tools must be on PATH. Install the `nlohmann_json` CMake package (3.11+) in the dependency prefix if it is missing. The automated build installs it for you. To refresh the AV1 software decoder in an older prefix, run `NDK="$NDK_ROOT" bash deps-android/build.sh arm64-v8a aom ffmpeg --clean` before packaging.

### Native Windows Build

#### 1. Tool and Dependency Layout
The Windows build script [scripts/build-frontend-apk-windows.ps1](scripts/build-frontend-apk-windows.ps1) accepts `-ToolsRoot` (defaults to a `build-tools` directory adjacent to the repository):

```text
build-tools/
  android-sdk/                  # Contains platforms/android-36 and build-tools/36.0.0
  ndk/android-ndk-r30/           # Windows NDK r30
  Qt/6.11.1/android_arm64_v8a/  # Qt Android ARM64
  Qt/6.11.1/msvc2022_64/        # Host Qt with bin/androiddeployqt.exe
  jdk-17.0.12/                  # JDK 17
  gradle/gradle-9.3.1/          # Gradle
  cmake/cmake-3.31.6-windows-x86_64/
  ninja/                        # ninja.exe
  android-deps-ffmpeg8/arm64-v8a/       # Precompiled ARM64 native headers and libraries
```

The dependency prefix must include the full Android ARM64 dependency set and the `nlohmann_json` CMake package (3.11+). Use `-NativeDeps` to point at another prefix. If an older APK lacks the libaom AV1 decoder, rebuild both `aom` and `ffmpeg` with [the Windows dependency script](scripts/build-deps-windows.sh).

#### 2. Compiling and Packaging
Execute the build script in PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-frontend-apk-windows.ps1 -Action all -ToolsRoot D:/build-tools
```

* `-Action configure`: Toolchain discovery and CMake configuration check.
* `-Action build`: Native C/C++ compilation only.
* `-Action package`: Incremental native build, asset staging, Qt IME patching, Gradle packaging, 16 KB alignment and signing.
* `-Action all`: Executes the entire end-to-end pipeline. Output APK is placed in `releases/`.

### Signing and Release Packaging

The Windows script accepts `-DebugSigning` for local testing with the repository’s public development key. The WSL script uses that key when signing is needed. PR artifacts also use the public development key.

For release signing:
1. Configure the `OBS_ANDROID_SIGNING_CONFIG` environment variable or pass `-SigningConfigFile`.
2. The configuration file (JSON or key-value format) defines `ANDROID_KEYSTORE_BASE64`, `ANDROID_KEY_ALIAS`, `ANDROID_KEYSTORE_PASSWORD`, and `ANDROID_KEY_PASSWORD`.
3. Keystore passwords are securely passed to `apksigner` via process environment variables and are never logged or committed.

### Optional Windows application ID

Pass `-AntutuPackage` to stage the alternate application ID `com.antutu.ABenchMark` for a Windows test build. Android installs it separately from `com.obsproject.studio`. Java and JNI classes retain the `com.obsproject.studio` namespace.

---

<a name="简体中文"></a>
## 简体中文

本文档指导如何在 Ubuntu / WSL、GitHub Actions CI 以及原生 Windows 环境下编译 OBS for Android。

* **目标架构**：`arm64-v8a`
* **API 等级**：最低支持 Android 10 (API 29)，编译 SDK 36，目标 SDK 35
* **默认包名**：`com.obsproject.studio`

### 构建方式概览

| 构建途径 | 环境要求 | 输出产物 | 适用场景 |
| --- | --- | --- | --- |
| **GitHub Actions** | 托管 Ubuntu 运行器 | 工作流产物 `obs-android-apk-<sha>` | 持续集成与自动化流水线 |
| **Ubuntu / WSL 自动化脚本** | Ubuntu 24.04 (或 WSL2) | `build-fe-arm64-v8a-ci/` 下的安装包 | 本地 Linux 一键全自动编译工具链与依赖 |
| **WSL / Linux 本地现有工具链** | 已配置 NDK r30 与 Qt 6.11.1 | `releases/OBS-Android-arm64-v8a-wsl-*.apk` | 日常本地增量开发与调试 |
| **Windows 原生环境** | Windows 10/11 + MSYS2 + PowerShell | `releases/OBS-Android-arm64-obs-*.apk` | 原生 Windows 交叉编译与自动化打包 |

### 基础工具链要求

* **Android SDK**：Build-Tools `36.0.0`，Platform `android-36`，Platform-Tools (ADB)
* **Android NDK**：`r30` (30.0.16248370)
* **Qt**：`6.11.1` (Android arm64 预编译包 + 对应宿主工具，包含 `androiddeployqt`)
* **Java**：JDK 17
* **构建系统**：CMake 3.31+ 与 Ninja
* **Gradle**：9.3.1
* **宿主依赖库**：Python 3，MSYS2 环境（用于 Windows 下编译部分 C/C++ 原生依赖）

依赖版本的完整定义见 [deps-android/versions.sh](deps-android/versions.sh)。

### Ubuntu / WSL 快速构建

在 Ubuntu 24.04 或 WSL2 环境下运行自动化构建脚本：

```sh
sudo apt-get update
sudo apt-get install -y 7zip ninja-build build-essential curl unzip zip ca-certificates \
  patch pkg-config dpkg-dev autoconf automake libtool nasm yasm python3-venv openjdk-17-jdk \
  libgl1-mesa-dev libegl1-mesa-dev libxkbcommon-dev

python3 -m venv "$HOME/.venvs/obs-android"
source "$HOME/.venvs/obs-android/bin/activate"
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64

bash scripts/ci-build-android.sh
cat build-fe-arm64-v8a-ci/.final-apk-path
```

### WSL / Linux 已有工具链

按 [已有工具链命令](#existing-toolchain) 设置 SDK、NDK、Qt、JDK 和 Gradle 路径。先构建 `deps-android/prebuilt/arm64-v8a/` 依赖前缀，再运行 WSL 打包脚本。若前缀缺少 `nlohmann_json` CMake 包，需先安装 3.11 或更新版本。自动化脚本会安装该依赖。旧前缀需要同时重编 AOM 和 FFmpeg 才能加入 AV1 软解。

### Windows 环境原生构建

#### 1. 工具与依赖目录结构
Windows 构建脚本 [scripts/build-frontend-apk-windows.ps1](scripts/build-frontend-apk-windows.ps1) 接受 `-ToolsRoot` 参数（默认为同级或指定路径下的 `build-tools` 目录）：

```text
build-tools/
  android-sdk/                  # 包含 platforms/android-36 与 build-tools/36.0.0
  ndk/android-ndk-r30/           # Windows 版 NDK r30
  Qt/6.11.1/android_arm64_v8a/  # Qt Android ARM64
  Qt/6.11.1/msvc2022_64/        # Qt 宿主工具 (含 androiddeployqt.exe)
  jdk-17.0.12/                  # JDK 17
  gradle/gradle-9.3.1/          # Gradle
  cmake/cmake-3.31.6-windows-x86_64/
  ninja/                        # ninja.exe
  android-deps-ffmpeg8/arm64-v8a/       # 预编译好的 ARM64 原生头文件与库
```

依赖前缀须包含完整的 Android ARM64 依赖与 `nlohmann_json` CMake 包（3.11 或更新版本）。可用 `-NativeDeps` 指定其他前缀。旧包缺少 libaom AV1 软解时，使用 [Windows 依赖脚本](scripts/build-deps-windows.sh) 同时重编 `aom` 与 `ffmpeg`。

#### 2. 编译与打包
在 PowerShell 中执行构建命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-frontend-apk-windows.ps1 -Action all -ToolsRoot D:/build-tools
```

* `-Action configure`：仅进行工具链探测与 CMake 配置检查；
* `-Action build`：仅执行 C/C++ 原生模块编译；
* `-Action package`：增量编译、资源暂存、Qt 补丁、Gradle 打包、16 KB 对齐与签名。
* `-Action all`：一键执行完整流程。最终安装包输出至 `releases/` 目录。

### 签名与发布配置

Windows 测试包可用 `-DebugSigning` 选择仓库公开开发密钥。WSL 脚本在需要签名时使用该密钥，PR 构建也使用公开开发签名。

若需进行正式发布签名：
1. 配置环境变量 `OBS_ANDROID_SIGNING_CONFIG` 或通过参数 `-SigningConfigFile` 指定外部签名配置文件；
2. 配置文件可为 JSON 或键值对格式，包含 `ANDROID_KEYSTORE_BASE64`、`ANDROID_KEY_ALIAS`、`ANDROID_KEYSTORE_PASSWORD`、`ANDROID_KEY_PASSWORD`；
3. 签名过程安全调用 `apksigner`，密钥信息不会记录在日志或提交至仓库。

### Windows 可选应用 ID

Windows 测试构建可通过 `-AntutuPackage` 将暂存的应用 ID 切换为 `com.antutu.ABenchMark`。Android 将其作为独立应用安装。Java/JNI 命名空间仍为 `com.obsproject.studio`。
