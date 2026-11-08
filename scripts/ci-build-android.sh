#!/usr/bin/env bash
# CI 用的一站式 Android APK 构建（在 Ubuntu runner 上验证）。
#
#   bash scripts/ci-build-android.sh               完整：工具链 → 依赖 → OBS → 打包 → 签名
#   bash scripts/ci-build-android.sh configure     只跑 CMake configure（快速验证 CMake 改动）
#
# 与 scripts/build-frontend-apk-wsl.sh 的分工：那个假设工具链**已装好**（读 ~/.obs-android-env.sh），
# 是给开发机用的；这个自己把工具链拉下来，所以能在裸 runner 上跑。
#
# 依赖（FFmpeg 等 14 个）不由这里下载二进制 —— 由 deps-android 从源码构建到
# deps-android/prebuilt/，workflow 里用 actions/cache 缓存。首次构建最慢，命中后很快。
#
# 环境变量（都有默认值）：
#   NDK_VERSION     默认 r30
#   QT_VERSION      默认 6.9.3
#   CMAKE_VERSION   默认 3.31.6（必须 >= 3.28：obs-studio 的 cmake_minimum_required 是 3.28...3.30）
#   CMDLINE_TOOLS   默认 13114758
#   GRADLE_VERSION  默认 8.14.3
#   BUILD_TOOLS     默认 36.0.0
#   PLATFORM        默认 android-35
#   JOBS            默认 nproc
#   TOOLS_ROOT      工具链落盘位置（workflow 缓存它），默认 $HOME/android-ci-tools
#   DL_ROOT         下载物落盘位置（**不**缓存），默认 $TOOLS_ROOT-dl
#   SIGN_KEYSTORE   给了且文件存在就签名；否则只出未签名包
#   SIGN_KEY_ALIAS / SIGN_STORE_PASS / SIGN_KEY_PASS  签名三件套
set -euo pipefail

STEP="${1:-all}"
ABI=arm64-v8a
API="${API:-29}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${OBS_SOURCE_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"

NDK_VERSION="${NDK_VERSION:-r30}"
QT_VERSION="${QT_VERSION:-6.9.3}"
CMAKE_VERSION="${CMAKE_VERSION:-3.31.6}"
CMDLINE_TOOLS="${CMDLINE_TOOLS:-13114758}"
GRADLE_VERSION="${GRADLE_VERSION:-8.14.3}"
BUILD_TOOLS="${BUILD_TOOLS:-36.0.0}"
PLATFORM="${PLATFORM:-android-35}"
JOBS="${JOBS:-$(nproc)}"
TOOLS_ROOT="${TOOLS_ROOT:-$HOME/android-ci-tools}"
DL_ROOT="${DL_ROOT:-$TOOLS_ROOT-dl}"

NDK="$TOOLS_ROOT/ndk/android-ndk-$NDK_VERSION"
QT="$TOOLS_ROOT/Qt/$QT_VERSION"
HOST="$QT/gcc_64"
SDK="$TOOLS_ROOT/android-sdk"
GRADLE_HOME="$TOOLS_ROOT/gradle-$GRADLE_VERSION"
CMAKE_HOME="$TOOLS_ROOT/cmake-$CMAKE_VERSION-linux-x86_64"
BUILD_DIR="$ROOT/build-fe-$ABI-ci"
PREFIX="$ROOT/deps-android/prebuilt/$ABI"

export ANDROID_SDK_ROOT="$SDK"
export ANDROID_HOME="$SDK"
export ANDROID_NDK_ROOT="$NDK"
export ANDROID_NDK_HOME="$NDK"
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk-amd64}"
export GRADLE_USER_HOME="$TOOLS_ROOT/gradle-cache"
export PATH="$CMAKE_HOME/bin:$GRADLE_HOME/bin:$SDK/platform-tools:$SDK/cmdline-tools/latest/bin:$JAVA_HOME/bin:$PATH"

log() { echo ""; echo "########## $* ##########"; }

# 7-Zip 的二进制名随发行版而变：Ubuntu 24.04 的 7zip 包给 7zz 与 7z/7za/7zr，
# 旧版 p7zip 给 7z。Qt 的 .7z 必须解包，所以这里把常见名字都认一遍，不写死。
SEVENZIP=""
find_sevenzip() {
    local c
    for c in 7z 7zz 7za 7zr; do
        if command -v "$c" >/dev/null 2>&1; then
            SEVENZIP="$c"
            return 0
        fi
    done
    echo "FATAL: 找不到 7-Zip 解包器（试过 7z/7zz/7za/7zr）。" >&2
    echo "       Ubuntu 24.04：sudo apt-get install -y 7zip" >&2
    echo "       （注意 p7zip-full 在 24.04 已是 transitional 包，不提供 7z）" >&2
    return 1
}

# ---------------------------------------------------------------- 工具链

fetch_cmake() {
    # 系统自带的可能低于 3.28（Ubuntu 22.04 是 3.22），而 obs-studio 要求 3.28+。
    # 用一个固定版本、随工具链缓存，避免"runner 换镜像导致 CMake 版本漂移"。
    if [ -x "$CMAKE_HOME/bin/cmake" ]; then
        echo "CMake 已就位：$("$CMAKE_HOME/bin/cmake" --version | head -1)"
        return
    fi
    log "下载 CMake $CMAKE_VERSION"
    mkdir -p "$TOOLS_ROOT" "$DL_ROOT"
    local tgz="$DL_ROOT/cmake-$CMAKE_VERSION.tar.gz"
    [ -s "$tgz" ] || curl -fL --retry 3 --retry-delay 5 -o "$tgz" \
        "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-linux-x86_64.tar.gz"
    tar -xzf "$tgz" -C "$TOOLS_ROOT"
    [ -x "$CMAKE_HOME/bin/cmake" ] || { echo "FATAL: CMake 解包后没找到 $CMAKE_HOME/bin/cmake" >&2; exit 1; }
}

fetch_ndk() {
    [ -d "$NDK" ] && { echo "NDK 已就位：$NDK"; return; }
    log "下载 NDK $NDK_VERSION"
    mkdir -p "$TOOLS_ROOT/ndk" "$DL_ROOT"
    local zip="$DL_ROOT/android-ndk-$NDK_VERSION-linux.zip"
    [ -s "$zip" ] || curl -fL --retry 3 --retry-delay 5 -o "$zip" \
        "https://dl.google.com/android/repository/android-ndk-$NDK_VERSION-linux.zip"
    unzip -q "$zip" -d "$TOOLS_ROOT/ndk"
    rm -f "$zip"
    [ -d "$NDK" ] || { echo "FATAL: 解压后没找到 $NDK" >&2; exit 1; }
}

fetch_qt() {
    [ -x "$HOST/bin/androiddeployqt" ] && { echo "Qt 已就位：$QT"; return; }
    log "下载 Qt $QT_VERSION（Android arm64 目标 + Linux gcc_64 宿主）"
    # 目录布局：桌面宿主在 linux_x64/desktop；Android 目标是 all_os/android。
    # 包名前缀是**完整的 6.9.3-0-<时间戳>**（不只时间戳），宿主与目标各一套时间戳，
    # 必须是同一批产物（混用会在 configure 时报 Qt6 版本/工具链不匹配）。
    # 目标包名带 "MacOS" 字样是 Qt 的打包命名习惯 —— 它是跨平台产物，与谁来构建无关
    #（本仓 Windows 构建用的也是同一个包）。
    #
    # 下面这几条 URL 都是**实测可下载**的（HTTP 206 + 真实字节），改动前请重新验证：
    #   https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/qt6_693/qt6_693/qt.qt6.693.linux_gcc_64/
    #   https://download.qt.io/online/qtsdkrepository/all_os/android/qt6_693/qt6_693_arm64_v8a/
    local host_tag="${QT_VERSION}-0-202509261207"
    local and_tag="${QT_VERSION}-0-202509261208"
    local dst="$QT"
    mkdir -p "$DL_ROOT" "$dst/android_arm64_v8a" "$dst/gcc_64"

    qt_get() { # url, outfile
        [ -s "$2" ] || curl -fL --retry 3 --retry-delay 5 -o "$2" "$1"
    }
    local host_base="https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/qt6_693/qt6_693"
    local and_base="https://download.qt.io/online/qtsdkrepository/all_os/android/qt6_693/qt6_693_arm64_v8a"
    local host_tail="Linux-RHEL_8_10-GCC-Linux-RHEL_8_10-X86_64.7z"
    local and_tail="MacOS-MacOS_13-Clang-Android-Android_ANY-ARM64.7z"

    qt_get "$host_base/qt.qt6.693.linux_gcc_64/${host_tag}qtbase-$host_tail" \
           "$DL_ROOT/qtbase-host.7z"
    qt_get "$host_base/qt.qt6.693.linux_gcc_64/${host_tag}qtsvg-$host_tail" \
           "$DL_ROOT/qtsvg-host.7z"
    qt_get "$host_base/qt.qt6.693.addons.qtnetworkauth.linux_gcc_64/${host_tag}qtnetworkauth-$host_tail" \
           "$DL_ROOT/qtnetworkauth-host.7z"
    qt_get "$and_base/qt.qt6.693.android_arm64_v8a/${and_tag}qtbase-$and_tail" \
           "$DL_ROOT/qtbase-android.7z"
    qt_get "$and_base/qt.qt6.693.android_arm64_v8a/${and_tag}qtsvg-$and_tail" \
           "$DL_ROOT/qtsvg-android.7z"
    qt_get "$and_base/qt.qt6.693.addons.qtnetworkauth.android_arm64_v8a/${and_tag}qtnetworkauth-$and_tail" \
           "$DL_ROOT/qtnetworkauth-android.7z"

    local f
    for f in qtbase-host qtsvg-host qtnetworkauth-host; do
        echo "  解包 $f → gcc_64"
        "$SEVENZIP" x -y -o"$dst/gcc_64" "$DL_ROOT/$f.7z" >/dev/null
    done
    for f in qtbase-android qtsvg-android qtnetworkauth-android; do
        echo "  解包 $f → android_arm64_v8a"
        "$SEVENZIP" x -y -o"$dst/android_arm64_v8a" "$DL_ROOT/$f.7z" >/dev/null
    done

    # Android 包里的 target_qt.conf 指向 Qt 官方构建机，必须改指到我们这份 Linux 宿主，
    # 否则 androiddeployqt 找不到宿主的 moc/rcc/uic（Linux Qt6 把它们放在 libexec 下）。
    # 本仓 setup-qt-6.9.3.ps1 对 Windows 做过同一件事（那边指向 msvc2022_64、且 libexec→bin）。
    local conf="$dst/android_arm64_v8a/bin/target_qt.conf"
    if [ -f "$conf" ]; then
        sed -i 's|^HostPrefix=.*|HostPrefix=../../gcc_64|' "$conf"
        sed -i 's|^HostLibraryExecutables=.*|HostLibraryExecutables=libexec|' "$conf"
        sed -i 's|^HostData=.*|HostData=.|' "$conf"
        echo "  已改写 target_qt.conf → 宿主 $HOST"
    fi
    [ -x "$HOST/bin/androiddeployqt" ] || { echo "FATAL: Qt 宿主缺 androiddeployqt（解包不完整？）" >&2; exit 1; }
    [ -x "$HOST/libexec/moc" ] || [ -x "$HOST/bin/moc" ] || \
        echo "WARN: 宿主 moc 不在常见位置，androiddeployqt 可能找不到"
}

fetch_sdk() {
    if [ ! -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]; then
        log "下载 Android SDK commandline-tools"
        mkdir -p "$SDK/cmdline-tools" "$DL_ROOT"
        local zip="$DL_ROOT/cmdline-tools.zip"
        [ -s "$zip" ] || curl -fL --retry 3 --retry-delay 5 -o "$zip" \
            "https://dl.google.com/android/repository/commandlinetools-linux-${CMDLINE_TOOLS}_latest.zip"
        unzip -q "$zip" -d "$SDK/cmdline-tools"
        rm -f "$zip"
        # 官方要求目录名恰好是 latest，否则 sdkmanager 认为布局不对。
        [ -d "$SDK/cmdline-tools/latest" ] || mv "$SDK/cmdline-tools/cmdline-tools" "$SDK/cmdline-tools/latest"
    fi
    log "安装 SDK 组件（$PLATFORM / build-tools $BUILD_TOOLS / platform-tools）"
    yes 2>/dev/null | "$SDK/cmdline-tools/latest/bin/sdkmanager" --licenses >/dev/null 2>&1 || true
    "$SDK/cmdline-tools/latest/bin/sdkmanager" --install \
        "platform-tools" "platforms;$PLATFORM" "build-tools;$BUILD_TOOLS" >/dev/null
    # 让 SDK 布局里也能按 revision 找到 NDK（Gradle/AGP 会去 $SDK/ndk/<rev> 找）。
    # 不重复下载几 GB：直接把已解包的 NDK 软链进去。
    local rev
    rev="$(sed -n 's/^Pkg\.Revision *= *//p' "$NDK/source.properties" | tr -d ' \r')"
    if [ -n "$rev" ]; then
        mkdir -p "$SDK/ndk"
        ln -sfn "$NDK" "$SDK/ndk/$rev"
    fi
}

fetch_gradle() {
    [ -x "$GRADLE_HOME/bin/gradle" ] && { echo "Gradle 已就位：$GRADLE_HOME"; return; }
    log "下载 Gradle $GRADLE_VERSION"
    mkdir -p "$TOOLS_ROOT" "$DL_ROOT"
    local zip="$DL_ROOT/gradle.zip"
    [ -s "$zip" ] || curl -fL --retry 3 --retry-delay 5 -o "$zip" \
        "https://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip"
    unzip -q "$zip" -d "$TOOLS_ROOT"
    rm -f "$zip"
}

# ---------------------------------------------------------------- 依赖

build_deps() {
    # 两个哨兵：FFmpeg（core 批次尾部）与 libusb（usb 批次）。
    # 缺任何一个都说明缓存不完整，重新构建（deps 脚本自己会按依赖名跳过已完成的）。
    if [ -f "$PREFIX/lib/libavcodec.a" ] && [ -f "$PREFIX/lib/libusb-1.0.a" ]; then
        echo "依赖产物已在位：$PREFIX（缓存命中，跳过源码构建）"
        return
    fi
    log "拉取依赖源码"
    ( cd "$ROOT/deps-android" && bash fetch.sh )
    log "交叉编译依赖（含 FFmpeg/x264，最慢的一环）"
    ( cd "$ROOT/deps-android" && NDK="$NDK" API="$API" JOBS="$JOBS" bash build.sh "$ABI" )
    for lib in libavcodec.a libusb-1.0.a libturbojpeg.a; do
        [ -f "$PREFIX/lib/$lib" ] || { echo "FATAL: 依赖缺失：$PREFIX/lib/$lib" >&2; exit 1; }
    done
}

# ---------------------------------------------------------------- OBS

configure_obs() {
    log "CMake configure"
    rm -rf "$BUILD_DIR"
    cmake -S "$ROOT/obs-studio" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="android-$API" \
        -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_PREFIX_PATH="$PREFIX;$QT/android_arm64_v8a" \
        -DQT_HOST_PATH="$HOST" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DENABLE_PLUGINS=ON \
        -DENABLE_FRONTEND=ON \
        -DENABLE_SCRIPTING=OFF \
        -DENABLE_HEVC=ON \
        -DANDROID_SDK_ROOT="$SDK" \
        -DCMAKE_INSTALL_PREFIX="$BUILD_DIR/install"
}

build_obs() {
    log "构建整树（-j$JOBS）"
    cmake --build "$BUILD_DIR" -j"$JOBS"
}

package_apk() {
    log "暂存插件 .so 进包源目录"
    local pkg_libs="$ROOT/obs-studio/frontend/cmake/android/libs/$ABI"
    local plug_src
    plug_src="$(find "$BUILD_DIR/rundir" -type d -path '*/lib/obs-plugins' 2>/dev/null | head -1)"
    if [ -z "$plug_src" ]; then
        echo "FATAL: rundir 里没有 obs-plugins 目录 —— 插件没编出来，APK 会是空壳" >&2
        exit 1
    fi
    mkdir -p "$pkg_libs"
    cp -f "$plug_src"/*.so "$pkg_libs"/
    # 这两个是本 fork 的核心插件。缺了就早点报错，别出一个"看着正常"的包。
    local m
    for m in android-mediacodec.so android-browser.so; do
        [ -f "$pkg_libs/$m" ] || { echo "FATAL: 插件缺失：$m" >&2; exit 1; }
    done
    echo "  已暂存：$(ls "$pkg_libs" | tr '\n' ' ')"

    log "androiddeployqt 出包"
    local ab_dir="$BUILD_DIR/frontend/android-build"
    local fe="$BUILD_DIR/frontend"
    mkdir -p "$ab_dir/libs/$ABI"
    cp -f "$plug_src"/*.so "$ab_dir/libs/$ABI"/
    # 强制重打包：清掉 androiddeployqt/gradle 的 native 中间件，否则上一轮暂存的 .so 会残留。
    rm -rf "$ab_dir"/build/outputs/apk \
           "$ab_dir"/build/intermediates/merged_jni_libs \
           "$ab_dir"/build/intermediates/merged_native_libs \
           "$ab_dir"/build/intermediates/stripped_native_libs
    ( cd "$fe" && "$HOST/bin/androiddeployqt" \
        --input "$fe/android-obs-studio-deployment-settings.json" \
        --output "$ab_dir" \
        --apk "$ab_dir/obs-studio.apk" \
        --depfile "$ab_dir/obs-studio.d" \
        --builddir "$fe" --release )

    local apk
    apk="$(find "$ab_dir/build/outputs/apk/release" -name '*-unsigned.apk' 2>/dev/null | head -1)"
    [ -n "$apk" ] || apk="$(find "$BUILD_DIR" -name '*-unsigned.apk' 2>/dev/null | head -1)"
    [ -n "$apk" ] || apk="$(find "$ab_dir" -name '*.apk' ! -name '*-signed*' 2>/dev/null | head -1)"
    [ -n "$apk" ] || { echo "FATAL: 没找到生成的 APK" >&2; exit 1; }
    echo "$apk" > "$BUILD_DIR/.unsigned-apk-path"
    echo "  未签名 APK：$apk"
}

sign_apk() {
    local apk out
    apk="$(cat "$BUILD_DIR/.unsigned-apk-path")"
    out="$BUILD_DIR/OBS-Android-$ABI-release.apk"

    if [ -z "${SIGN_KEYSTORE:-}" ] || [ ! -f "$SIGN_KEYSTORE" ]; then
        log "未提供签名密钥 —— 只产出未签名 APK"
        echo "  提示：release 包必须签名才能安装。本地先生成密钥（scripts/generate-release-key.sh），"
        echo "        或把 base64 密钥配进 GitHub Secrets，然后重跑。"
        echo "$apk" > "$BUILD_DIR/.final-apk-path"
        return
    fi

    # 少了任何一件签名参数都不要"先出个包"——那会产出一个装不上的 release APK，
    # 而且失败会晚到用户手上才暴露。
    : "${SIGN_KEY_ALIAS:?给了 SIGN_KEYSTORE 就必须给 SIGN_KEY_ALIAS}"
    : "${SIGN_STORE_PASS:?给了 SIGN_KEYSTORE 就必须给 SIGN_STORE_PASS}"
    : "${SIGN_KEY_PASS:?给了 SIGN_KEYSTORE 就必须给 SIGN_KEY_PASS}"

    log "zipalign（-P 16：16 KB 页对齐，Android 15+ 对 .so 的硬要求）"
    local aligned="$BUILD_DIR/aligned.apk"
    "$SDK/build-tools/$BUILD_TOOLS/zipalign" -f -P 16 4 "$apk" "$aligned"

    log "apksigner 签名"
    "$SDK/build-tools/$BUILD_TOOLS/apksigner" sign \
        --ks "$SIGN_KEYSTORE" \
        --ks-key-alias "$SIGN_KEY_ALIAS" \
        --ks-pass "pass:$SIGN_STORE_PASS" \
        --key-pass "pass:$SIGN_KEY_PASS" \
        --out "$out" "$aligned"

    log "校验（签名 + 对齐）"
    "$SDK/build-tools/$BUILD_TOOLS/apksigner" verify --verbose "$out"
    "$SDK/build-tools/$BUILD_TOOLS/zipalign" -c -P 16 -v 4 "$out" | tail -3

    echo "$out" > "$BUILD_DIR/.final-apk-path"
    echo "  已签名 APK：$out"
    sha256sum "$out"
    du -h "$out" | cut -f1 | xargs echo "  大小："
}

# ---------------------------------------------------------------- main

find_sevenzip

log "环境"
echo "repo      : $ROOT"
echo "tools     : $TOOLS_ROOT"
echo "downloads : $DL_ROOT"
echo "ndk       : $NDK_VERSION"
echo "qt        : $QT_VERSION"
echo "cmake     : $CMAKE_VERSION"
echo "gradle    : $GRADLE_VERSION"
echo "sdk       : $SDK  platform=$PLATFORM build-tools=$BUILD_TOOLS"
echo "jobs      : $JOBS"
echo "7-zip     : $SEVENZIP"
java -version 2>&1 | head -1

fetch_cmake
fetch_ndk
fetch_qt
fetch_sdk
fetch_gradle

cmake --version | head -1

build_deps

configure_obs
[ "$STEP" = "configure" ] && { log "只配置，完成"; exit 0; }

build_obs
package_apk
sign_apk

log "完成"
