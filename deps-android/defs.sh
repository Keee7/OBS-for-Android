#!/bin/bash
# 各依赖的交叉编译定义。由 build.sh source 后按 build_<dep> 约定调用。
#
# 前提：build.sh 已导出
#   ABI API NDK TOOLCHAIN SYSROOT TRIPLE CC CXX AR AS RANLIB STRIP NM OBJCOPY OBJDUMP LD
#   PREFIX JOBS BUILD_TRIPLE src_dir()
#
# 统一策略：全部编成 **静态库**（-fPIC），OBS 侧链接进各 .so；zlib 直接用 NDK sysroot。
set -uo pipefail

_common_flags() {
  COMMON_CFLAGS="-O2 -fPIC -I${PREFIX}/include"
  COMMON_LDFLAGS="-L${PREFIX}/lib -Wl,-z,max-page-size=16384"
}

_common_env() {
  _common_flags
  export CC CXX AR AS RANLIB STRIP NM OBJCOPY OBJDUMP LD
  export CFLAGS="$COMMON_CFLAGS" CXXFLAGS="$COMMON_CFLAGS" LDFLAGS="$COMMON_LDFLAGS"
  export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
  export PKG_CONFIG_SYSROOT_DIR=""
}

# ---------------------------------------------------------------- header-only
# 这两个是 header-only：没有 make/cmake 的显式失败码，若 cp 失败（源码包解压不全、
# 权限、磁盘）而最后一句 echo 成功，整个函数会返回 0，build.sh 就会打上永久 stamp。
# 因此每条可能失败的命令都显式检查返回值。
build_simde() {
  local s; s=$(src_dir simde)
  mkdir -p "$PREFIX/include" || return 1
  rm -rf "$PREFIX/include/simde" || return 1
  cp -r "$s/simde" "$PREFIX/include/" || return 1
  echo "       SIMDe 头文件已安装 (header-only)"
}

build_uthash() {
  local s; s=$(src_dir uthash)
  mkdir -p "$PREFIX/include" || return 1
  cp "$s/src/"*.h "$PREFIX/include/" || return 1
  echo "       uthash 头文件已安装 (header-only)"
}

# ---------------------------------------------------------------- cmake 系
_run_cmake() {
  local s=$1; shift
  # AOM ships required build/cmake/*.cmake as source. Never use or clear
  # the upstream build/ directory as our generated CMake binary directory.
  local b="$s/.obs-android-build"
  _common_env
  # 交叉 pkg-config：只准在 $PREFIX 里找，避免误用宿主 /usr/lib 的库
  export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
  # CMake 系依赖的编译器由 NDK toolchain 直接指定为绝对路径，$CC 环境变量
  # 对它们无效，所以要显式挂 launcher（与 OBS 主构建一致）。
  local -a cc_launcher=()
  if [ "${USE_CCACHE:-0}" = "1" ] && command -v ccache >/dev/null 2>&1; then
    cc_launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  fi
  rm -rf "$b" || return 1
  cmake -S "$s" -B "$b" \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF \
    "${cc_launcher[@]}" \
    "$@" || return 1
  cmake --build "$b" -j"$JOBS" || return 1
  cmake --install "$b" || return 1
}

build_jansson() {
  _run_cmake "$(src_dir jansson)" \
    -DJANSON_BUILD_ZONED_TOOLS=OFF \
    -DJANSON_BUILD_RENDERER=OFF \
    -DJANSON_BUILD_DOC=OFF
}

build_libusb() {
  # libusb 1.0.28 源码树无 CMakeLists.txt（CMake 支持仅存在于 git master），走 autotools
  _autoconf_dep libusb --disable-udev --disable-examples-build
}

build_libjpeg_turbo() {
  # libuvc 的 MJPEG 解码依赖 libjpeg；turbojpeg 以 native 模式提供 libjpeg.a + jpeglib.h
  _run_cmake "$(src_dir libjpeg-turbo)" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
    -DWITH_JAVA=OFF -DWITH_ICC_LIB=OFF \
    -DWITH_TURBOJPEG=ON
}

build_libuvc() {
  # libuvc 依赖 libusb + libjpeg（两者须先构建）
  # 0.0.7 用 CMAKE_BUILD_TARGET 选产物类型（不认 BUILD_SHARED_LIBS），
  # 且 BUILD_EXAMPLE 默认 ON 会强制 find_package(OpenCV)，必须关掉
  _run_cmake "$(src_dir libuvc)" \
    -DCMAKE_BUILD_TARGET=Static \
    -DBUILD_EXAMPLE=OFF \
    -DBUILD_TEST=OFF
}

build_curl() {
  _run_cmake "$(src_dir curl)" \
    -DBUILD_CURL_EXE=OFF \
    -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
    -DENABLE_CURL_MANUAL=OFF \
    -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF -DCURL_USE_LIBPSL=OFF \
    -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
    -DCURL_USE_OPENSSL=OFF -DCURL_USE_MBEDTLS=ON \
    -DCURL_USE_SECOBJC=OFF \
    -DCMAKE_USE_MBEDTLS=ON \
    -DMBEDTLS_INCLUDE_DIR="$PREFIX/include" \
    -DMBEDTLS_LIBRARY="$PREFIX/lib/libmbedtls.a" \
    -DMBEDX509_LIBRARY="$PREFIX/lib/libmbedx509.a" \
    -DMBEDCRYPTO_LIBRARY="$PREFIX/lib/libmbedcrypto.a"
}

# ---------------------------------------------------------------- autotools / 自有 configure
# 通用：gnu 风格 configure + --host=$TRIPLE
_autoconf_dep() {
  local name=$1; shift
  local s; s=$(src_dir "$name")
  _common_env
  ( cd "$s" || exit 1
    [ -x ./configure ] || autoreconf -fi || exit 1
    ./configure --prefix="$PREFIX" --host="$TRIPLE" --build="$BUILD_TRIPLE" \
      --enable-static --disable-shared "$@" || exit 1
    make -j"$JOBS" || exit 1
    make install || exit 1
  )
}

build_rnnoise()  { _autoconf_dep rnnoise  --disable-examples; }
build_speexdsp() { _autoconf_dep speexdsp --disable-doc; }

build_freetype() {
  _autoconf_dep freetype \
    --without-harfbuzz --without-bzip2 --without-png --without-zlib --without-brotli
}

build_x264() {
  local s; s=$(src_dir x264)
  _common_env
  # x264 按架构自选汇编器（x86→nasm，aarch64→clang）；
  # 继承来的 AS=clang 会让 x86_64 分支拿 clang 去试 nasm 语法而直接失败
  unset AS
  ( cd "$s" || exit 1
    ./configure \
      --prefix="$PREFIX" \
      --host="$TRIPLE" \
      --cross-prefix="${TOOLCHAIN}/bin/${TRIPLE}-" \
      --sysroot="$SYSROOT" \
      --enable-static --enable-pic \
      --disable-cli \
      --disable-opencl --disable-lavf --disable-avs --disable-ffms \
      --disable-gpac --disable-lsmash --disable-interlaced \
      --extra-cflags="$COMMON_CFLAGS" \
      --extra-ldflags="$COMMON_LDFLAGS" || exit 1
    make -j"$JOBS" || exit 1
    make install-lib-static || make install || exit 1
  )
}

# Build CPU encoders before FFmpeg, as PIC static libraries.
build_x265() {
  local source patch_file
  source=$(src_dir x265)
  patch_file="$HERE/../scripts/android-native-deps/x265-3.6-android-asm-target.patch"
  if ! patch -d "$source" -p1 -R --force --dry-run --silent < "$patch_file" >/dev/null 2>&1; then
    patch -d "$source" -p1 --force < "$patch_file" || return 1
  fi
  _run_cmake "$(src_dir x265)/source" -DENABLE_SHARED=OFF -DENABLE_CLI=OFF \
    -DENABLE_ASSEMBLY=ON -DENABLE_TESTS=OFF -DHIGH_BIT_DEPTH=OFF || return 1
  # x265 3.6 writes invalid -l-l:libunwind.a entries with NDK r30.
  sed -i 's/^Libs.private:.*/Libs.private: -lc++_static -lc++abi -lunwind -lm -ldl/' "$PREFIX/lib/pkgconfig/x265.pc"
}

build_aom() {
  _run_cmake "$(src_dir aom)" -DENABLE_DOCS=OFF -DENABLE_TESTS=OFF \
    -DENABLE_EXAMPLES=OFF -DENABLE_TOOLS=OFF -DCONFIG_AV1_DECODER=0
}

# ---------------------------------------------------------------- ffmpeg
build_ffmpeg() {
  local s; s=$(src_dir ffmpeg)
  # Both dependency build routes must use the decoder fixes shipped with OBS.
  # codec_name 补丁已移除：媒体源硬件解码改为直接使用 FFmpeg 的 *_mediacodec 解码器
  #（具体组件由系统选择），这里只保留 byte-buffer 输出的 crop/format 修正。
  local patch_file
  for patch_file in "$HERE/../scripts/android-native-deps/ffmpeg-8.0.3-mediacodec-android.patch"; do
    if patch -d "$s" -p1 -R --force --dry-run --silent < "$patch_file" > /dev/null 2>&1; then
      echo "Already applied: $(basename "$patch_file")"
    elif patch -d "$s" -p1 --force --dry-run --silent < "$patch_file" > /dev/null 2>&1; then
      patch -d "$s" -p1 --force < "$patch_file" || return 1
    else
      echo "FFmpeg patch does not match source: $patch_file" >&2
      return 1
    fi
  done
  _common_env
  local arch extra
  case "$ABI" in
    arm64-v8a) arch=aarch64; extra="--enable-neon" ;;
    x86_64)    arch=x86_64;  extra="--disable-neon --enable-x86asm" ;;
    *) echo "未知 ABI: $ABI" >&2; return 1 ;;
  esac
  ( cd "$s" || exit 1
    ./configure \
      --prefix="$PREFIX" \
      --enable-cross-compile \
      --target-os=android \
      --arch="$arch" \
      --cc="$CC" --cxx="$CXX" --ar="$AR" --ranlib="$RANLIB" --strip="$STRIP" \
      --sysroot="$SYSROOT" \
      --enable-static --disable-shared \
      --disable-doc --disable-programs --disable-debug --disable-stripping \
      --disable-autodetect --enable-gpl --enable-libx265 --enable-libaom --disable-decoder=libaom_av1 \
      --pkg-config-flags=--static \
      --enable-mediacodec --enable-jni \
      --enable-avcodec --enable-avformat --enable-avutil \
      --enable-swscale --enable-swresample --enable-avfilter --enable-avdevice \
      --enable-zlib \
      --enable-asm --enable-inline-asm \
      --disable-vulkan --disable-v4l2-m2m --disable-vaapi --disable-vdpau \
      --optflags="-O2 -fPIC" \
      --extra-ldflags="$COMMON_LDFLAGS" \
      $extra || exit 1
    make -j"$JOBS" || exit 1
    make install || exit 1
  )
}

build_mbedtls() {
  # 产物 libmbedtls.a / libmbedx509.a / libmbedcrypto.a —— build_curl 依赖这三个名字
  _run_cmake "$(src_dir mbedtls)" \
    -DENABLE_TESTING=OFF \
    -DENABLE_PROGRAMS=OFF \
    -DGEN_FILES=OFF \
    -DMBEDTLS_FATAL_WARNINGS=OFF
}
