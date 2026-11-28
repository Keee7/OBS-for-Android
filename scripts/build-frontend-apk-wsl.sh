#!/bin/bash
# WSL 版前端 APK 构建（arm64 专用）。
#
#   bash scripts/build-frontend-apk-wsl.sh            构建 + 签名 + 校验
#   bash scripts/build-frontend-apk-wsl.sh configure  只跑 CMake configure（快速验证 CMake 改动）
#
# 与 Windows 版的差异：
#   - 路径全部在 WSL home（~/obsd），无 AGP 非 ASCII 路径问题，无需 /d/obsd 联接
#   - Qt 宿主是 linux_gcc_64（提供 androiddeployqt），目标是 android_arm64_v8a
#   - 只保留 arm64-v8a（真机阶段），不含装机步骤——真机用 adb 手动装
# 环境来自 ~/.obs-android-env.sh（工具链部署脚本生成）。
set -uo pipefail

ACT="${1:-build}"
API="${API:-29}"
ABI=arm64-v8a
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${OBS_SOURCE_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"

while IFS='=' read -r _k _v || [ -n "$_k" ]; do
  case "$_k" in ''|'#'*) continue ;; esac
  [ -n "${!_k+x}" ] || export "$_k=$_v"
done < "$SCRIPT_DIR/toolchain-versions.env"

[ -f "$HOME/.obs-android-env.sh" ] && . "$HOME/.obs-android-env.sh"

SDK="${ANDROID_SDK_ROOT:?缺 ANDROID_SDK_ROOT，先 source ~/.obs-android-env.sh}"
QT="${QT_DIR:?缺 QT_DIR}"
HOST="${QT_HOST_PATH:?缺 QT_HOST_PATH}"
NDK="${NDK_ROOT:?缺 NDK_ROOT}"
GRADLE_BIN="${GRADLE_BIN:-$HOME/toolchain/gradle-$GRADLE_VERSION/bin}"
COMPILE_SDK_VERSION="${OBS_ANDROID_COMPILE_SDK_VERSION:-$COMPILE_SDK_VERSION}"
BUILD_TOOLS_VERSION="${BUILD_TOOLS_VERSION:-36.0.0}"
case "$COMPILE_SDK_VERSION" in
  ''|*[!0-9]*) echo "FATAL: OBS_ANDROID_COMPILE_SDK_VERSION 必须为 SDK 平台版本号" >&2; exit 1 ;;
esac
[ -f "$SDK/platforms/android-$COMPILE_SDK_VERSION/android.jar" ] || {
  echo "FATAL: 缺少 SDK platform android-$COMPILE_SDK_VERSION，请先运行 sdkmanager \"platforms;android-$COMPILE_SDK_VERSION\"" >&2
  exit 1
}

export PATH="$HOME/.venv/bin:$GRADLE_BIN:$SDK/platform-tools:$PATH"
if [ -z "${JAVA_HOME:-}" ]; then
  for _jvm in "/usr/lib/jvm/java-${JDK_VERSION}-openjdk-amd64" "/usr/lib/jvm/java-17-openjdk-amd64"; do
    [ -x "$_jvm/bin/java" ] && { JAVA_HOME="$_jvm"; break; }
  done
  export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-${JDK_VERSION}-openjdk-amd64}"
fi

[ -d "$QT/android_arm64_v8a" ] || { echo "FATAL: 缺少 Qt for Android：$QT/android_arm64_v8a" >&2; exit 1; }
[ -d "$NDK" ] || { echo "FATAL: 缺少 NDK：$NDK" >&2; exit 1; }
[ -x "$JAVA_HOME/bin/java" ] || { echo "FATAL: 缺少 JDK：$JAVA_HOME" >&2; exit 1; }
[ -x "$GRADLE_BIN/gradle" ] || { echo "FATAL: 缺少 Gradle：$GRADLE_BIN" >&2; exit 1; }
command -v cmake >/dev/null || { echo "FATAL: PATH 里没有 cmake（venv 没生效？）" >&2; exit 1; }

BUILD_DIR="$ROOT/build-fe-$ABI-plugins"
LOGDIR="$ROOT/build-logs"
mkdir -p "$LOGDIR"
LOG="${LOG:-$LOGDIR/fe-$ABI-apk-build-wsl.log}"

# Windows 树留下的 CMakeCache 指向 Windows 路径，必须清掉重配
if [ -f "$BUILD_DIR/CMakeCache.txt" ] && grep -aq 'Windows\\\|C:/\|D:/' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
  echo "=== 检测到 Windows 侧残留的 CMakeCache，清空构建目录重配 ==="
  rm -rf "$BUILD_DIR"
fi

echo "########## WSL APK 构建 $(date '+%F %T') ##########"
echo "cmake : $(cmake --version | head -1)"
echo "ninja : $(ninja --version)"
echo "gradle: $(gradle --version 2>/dev/null | awk '/^Gradle /{print $2; exit}')"
echo "Qt    : $QT/android_arm64_v8a  host=$HOST"
echo "NDK   : $NDK"
echo "ABI   : $ABI  API=$API  构建目录=$BUILD_DIR"

# ---- configure ----
cmake -S "$ROOT/obs-studio" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="android-$API" \
  -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DHAS_INTERPROCEDURAL_OPTIMIZATION=OFF \
  -DCMAKE_PREFIX_PATH="$ROOT/deps-android/prebuilt/$ABI;$QT/android_arm64_v8a" \
  -DQT_HOST_PATH="$HOST" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DENABLE_PLUGINS=ON \
  -DENABLE_FRONTEND=ON \
  -DENABLE_SCRIPTING=OFF \
  -DENABLE_HEVC=ON \
  -DANDROID_SDK_ROOT="$SDK" \
  -DOBS_ANDROID_COMPILE_SDK_VERSION="$COMPILE_SDK_VERSION" \
  -DCMAKE_INSTALL_PREFIX="$BUILD_DIR/install" 2>&1 | tee "$LOGDIR/fe-$ABI-apk-configure-wsl.log" | tail -25
CFG=${PIPESTATUS[0]}
echo "=== configure exit=$CFG ==="
[ "$CFG" = 0 ] || exit 1
[ "$ACT" = "configure" ] && { echo "=== 只配置，已完成 ==="; exit 0; }

# ---- 整树构建 ----
echo "=== build 整树（日志 $LOG） ==="
cmake --build "$BUILD_DIR" -j"$(nproc)" -- -k 0 > "$LOG" 2>&1
BUILD_RC=$?
echo "=== build exit=$BUILD_RC  失败任务数=$(grep -c '^FAILED:' "$LOG") ==="
grep -E "error:|fatal error|FAILED:" "$LOG" | head -30
[ "$BUILD_RC" = 0 ] || { echo "FATAL: 整树构建失败，看 $LOG"; exit 1; }

# ---- 打包与预验（stage → androiddeployqt → Qt jar → Gradle → 验包） ----
PKG_SOURCE="$ROOT/obs-studio/frontend/cmake/android"
python3 "$SCRIPT_DIR/package-android-apk.py" \
  --build-dir "$BUILD_DIR" \
  --package-source "$PKG_SOURCE" \
  --abi "$ABI" \
  --qt-version "$QT_VERSION" \
  --qt-android-dir "$QT/android_arm64_v8a" \
  --qt-host-dir "$HOST" \
  --sdk "$SDK" \
  --ndk "$NDK" \
  --java-home "$JAVA_HOME" \
  --gradle "$GRADLE_BIN/gradle" \
  --android-platform "android-$COMPILE_SDK_VERSION" \
  --build-tools "$BUILD_TOOLS_VERSION" \
  --clean-intermediates \
  --verify-unsigned \
  --unsigned-apk-out "$BUILD_DIR/.unsigned-apk-path" || exit 1

APP_LIB="$BUILD_DIR/frontend/libobs_${ABI}.so"
RE="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
if [ -f "$APP_LIB" ]; then
  echo "=== 应用库 main 动态符号 ==="
  "$RE" --dyn-syms "$APP_LIB" | grep -w "main" || echo "FAIL：.dynsym 里没有 main"
fi

# ---- 对齐 + 签名 ----
APK="$(cat "$BUILD_DIR/.unsigned-apk-path")"
[ -f "$APK" ] || { echo "FATAL: 未找到生成的 APK：$APK" >&2; exit 1; }

ZIPALIGN="$SDK/build-tools/$BUILD_TOOLS_VERSION/zipalign"
APKSIGNER="$SDK/build-tools/$BUILD_TOOLS_VERSION/apksigner"
ALIGNED="$BUILD_DIR/aligned.apk"
SIGNED="$(dirname "$APK")/$(basename "${APK%.apk}")-signed.apk"
"$ZIPALIGN" -f -P 16 4 "$APK" "$ALIGNED" || { echo "FATAL: zipalign 失败"; exit 1; }
"$APKSIGNER" sign \
  --ks "$SCRIPT_DIR/signing/debug.keystore" \
  --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android \
  --out "$SIGNED" "$ALIGNED" || { echo "FATAL: apksigner 签名失败"; exit 1; }
"$APKSIGNER" verify --verbose "$SIGNED" || { echo "FATAL: 签名校验未通过"; exit 1; }
"$ZIPALIGN" -c -P 16 -v 4 "$SIGNED" | tail -3 || { echo "FATAL: 16KB 页对齐校验未通过"; exit 1; }
APK="$SIGNED"

echo "=== APK ==="
ls -la "$APK"
echo "=== APK 内的 native 库（$ABI）与清单 ==="
unzip -l "$APK" | grep -c "lib/$ABI/" | xargs echo "lib/$ABI 条目数="
AAPT2="$SDK/build-tools/$BUILD_TOOLS_VERSION/aapt2"
"$AAPT2" dump badging "$APK" 2>/dev/null | grep -E "^package:|application-label:|native-code"

# ---- 归档到 releases ----
REL="$ROOT/releases"
mkdir -p "$REL"
STAMP=$(date '+%m%d-%H%M')
cp -f "$APK" "$REL/OBS-Android-$ABI-wsl-$STAMP.apk"
echo "=== 已归档: $REL/OBS-Android-$ABI-wsl-$STAMP.apk ==="
md5sum "$APK" "$REL/OBS-Android-$ABI-wsl-$STAMP.apk"
echo "########## 完成 $(date '+%F %T') ##########"
