#!/usr/bin/env bash
# Build genuine Android ARM64 static libraries using MSYS2 and Windows LLVM.
# SYSROOT must be the complete Android NDK sysroot (headers and API 29 libraries).
# WORK_ROOT must be an absolute, writable path without spaces. SOURCES_ROOT and
# OUTPUT_PREFIX may contain spaces. Sources are prepared separately by the user.
set -euo pipefail
export PATH="/usr/bin:/clang64/bin:$PATH"
: "${WORK_ROOT:?Set WORK_ROOT to a writable absolute MSYS path without spaces}"
: "${SYSROOT:?Set SYSROOT to the complete NDK sysroot in MSYS path form}"
: "${SOURCES_ROOT:?Set SOURCES_ROOT to build-tools/sources in MSYS path form}"
: "${OUTPUT_PREFIX:?Set OUTPUT_PREFIX to build-tools/android-deps/arm64-v8a}"
case "$WORK_ROOT" in /*) ;; *) echo 'WORK_ROOT must be absolute' >&2; exit 1;; esac
case "$WORK_ROOT" in *' '*) echo 'WORK_ROOT must not contain spaces' >&2; exit 1;; esac
LLVM_BIN=${LLVM_BIN:-$SYSROOT/../bin}
case "$LLVM_BIN" in *' '*) echo 'LLVM_BIN must not contain spaces; use a short path or a junction to the NDK LLVM bin directory' >&2; exit 1;; esac
JOBS=${JOBS:-4}
API=${API:-29}
for dep in bash make tar sed cp cygpath patch sha256sum; do command -v "$dep" >/dev/null || { echo "Missing tool: $dep" >&2; exit 1; }; done
for exe in clang.exe llvm-ar.exe llvm-ranlib.exe llvm-strip.exe llvm-nm.exe llvm-strings.exe llvm-readelf.exe; do
  test -f "$LLVM_BIN/$exe" || { echo "Missing LLVM tool: $LLVM_BIN/$exe" >&2; exit 1; }
done
test -f "$SYSROOT/usr/lib/aarch64-linux-android/$API/libc.so" || { echo "Incomplete NDK sysroot: missing API $API libc.so" >&2; exit 1; }
test -f "$SOURCES_ROOT/x264/configure" || { echo 'Missing official x264 source' >&2; exit 1; }
test -f "$SOURCES_ROOT/x265_3.6/source/CMakeLists.txt" || { echo 'Missing x265 3.6 source' >&2; exit 1; }
test -f "$SOURCES_ROOT/libaom-3.10.0/CMakeLists.txt" || { echo 'Missing libaom 3.10.0 source' >&2; exit 1; }
test -f "$SOURCES_ROOT/ffmpeg-8.0.3/configure" || { echo 'Missing official FFmpeg 8.0.3 source' >&2; exit 1; }
mkdir -p "$WORK_ROOT" "$WORK_ROOT/src" "$WORK_ROOT/bin" "$WORK_ROOT/prefix" "$OUTPUT_PREFIX"
# Windows Clang receives a Windows-form sysroot. Keeping its path space-free is
# necessary because both projects expand compiler flags as shell words.
if [[ "$SYSROOT" != *' '* ]]; then
  build_sysroot="$SYSROOT"
else
  build_sysroot="$WORK_ROOT/sysroot"
  if [ ! -f "$build_sysroot/.copy-complete" ]; then
    mkdir -p "$build_sysroot"
    (cd "$SYSROOT" && tar -cf - .) | (cd "$build_sysroot" && tar -xf -)
    touch "$build_sysroot/.copy-complete"
  fi
fi
SYSROOT_NATIVE=$(cygpath -m "$build_sysroot")
LLVM_NATIVE=$(cygpath -m "$LLVM_BIN")
cat > "$WORK_ROOT/bin/android-cc" <<EOF
#!/usr/bin/env bash
exec "$LLVM_NATIVE/clang.exe" --target=aarch64-linux-android$API --sysroot="$SYSROOT_NATIVE" "\$@"
EOF
chmod +x "$WORK_ROOT/bin/android-cc"
export CC="$WORK_ROOT/bin/android-cc" AR="$LLVM_BIN/llvm-ar.exe" RANLIB="$LLVM_BIN/llvm-ranlib.exe"
export STRIP="$LLVM_BIN/llvm-strip.exe" NM="$LLVM_BIN/llvm-nm.exe"
export STRINGS="$LLVM_BIN/llvm-strings.exe"
export PKG_CONFIG_PATH="$WORK_ROOT/prefix/lib/pkgconfig" PKG_CONFIG_LIBDIR="$WORK_ROOT/prefix/lib/pkgconfig"
export CFLAGS="-O2 -fPIC -I$WORK_ROOT/prefix/include" LDFLAGS="-L$WORK_ROOT/prefix/lib -Wl,-z,max-page-size=16384"
prepare() {
  local source="$1" dest="$2"
  if [ ! -f "$dest/configure" ]; then
    mkdir -p "$dest"
    (cd "$source" && tar --exclude=.git -cf - .) | (cd "$dest" && tar -xf -)
  fi
}
prepare "$SOURCES_ROOT/x264" "$WORK_ROOT/src/x264"
patch_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/android-native-deps"
patch_file="$patch_dir/ffmpeg-8.0.3-mediacodec-android.patch"
patch_digest=$(sha256sum < "$patch_file")
ffmpeg_work="$WORK_ROOT/src/ffmpeg-8.0.3-${patch_digest%% *}"
prepare "$SOURCES_ROOT/ffmpeg-8.0.3" "$ffmpeg_work"
if patch -d "$ffmpeg_work" -p1 -R --force --dry-run --silent < "$patch_file" >/dev/null 2>&1; then
  echo "Already applied: $patch_file"
else
  patch -d "$ffmpeg_work" -p1 --force < "$patch_file"
fi
prefix="$WORK_ROOT/prefix"
targets=" ${*:-x264 x265 aom ffmpeg} "
# CMake's Android toolchain uses the original NDK, not the copied sysroot.
NDK_ROOT=${NDK_ROOT:-$(cd "$SYSROOT/../../../../.." && pwd)}
cmake_android() {
  local source=$1 build=$2; shift 2
  cmake -S "$(cygpath -m "$source")" -B "$(cygpath -m "$build")" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$NDK_ROOT/build/cmake/android.toolchain.cmake")" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-$API" -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$(cygpath -m "$prefix")" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
  cmake --build "$(cygpath -m "$build")" --parallel "$JOBS"
  cmake --install "$(cygpath -m "$build")"
}
if [[ "$targets" == *' x265 '* ]]; then
  mkdir -p "$WORK_ROOT/src/x265"
  if [ ! -f "$WORK_ROOT/src/x265/source/CMakeLists.txt" ]; then
    (cd "$SOURCES_ROOT/x265_3.6" && tar -cf - .) | (cd "$WORK_ROOT/src/x265" && tar -xf -)
  fi
  x265_patch="$patch_dir/x265-3.6-android-asm-target.patch"
  if ! patch -d "$WORK_ROOT/src/x265" -p1 -R --force --dry-run --silent < "$x265_patch" >/dev/null 2>&1; then
    patch -d "$WORK_ROOT/src/x265" -p1 --force < "$x265_patch"
  fi
  cmake_android "$WORK_ROOT/src/x265/source" "$WORK_ROOT/x265-build" \
    -DENABLE_SHARED=OFF -DENABLE_CLI=OFF -DENABLE_ASSEMBLY=ON -DENABLE_TESTS=OFF -DHIGH_BIT_DEPTH=OFF
  # Correct x265 3.6's malformed -l-l:libunwind.a entries under NDK r30.
  sed -i 's/^Libs.private:.*/Libs.private: -lc++_static -lc++abi -lunwind -lm -ldl/' "$prefix/lib/pkgconfig/x265.pc"
fi
if [[ "$targets" == *' aom '* ]]; then
  cmake_android "$SOURCES_ROOT/libaom-3.10.0" "$WORK_ROOT/aom-build" \
    -DENABLE_DOCS=OFF -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_TOOLS=OFF -DCONFIG_AV1_DECODER=1
fi
if [[ "$targets" == *' x264 '* ]]; then
  (cd "$WORK_ROOT/src/x264"
   unset AS
   ./configure --prefix="$prefix" --host=aarch64-linux-android --enable-static --enable-pic \
     --disable-cli --disable-opencl --disable-lavf --disable-avs --disable-ffms \
     --disable-gpac --disable-lsmash --disable-interlaced
   make -j"$JOBS"
   make install-lib-static)
fi
if [[ "$targets" == *' ffmpeg '* ]]; then
  (cd "$ffmpeg_work"
   ./configure --prefix="$prefix" --enable-cross-compile --target-os=android --arch=aarch64 \
     --cc="$CC" --ar="$AR" --ranlib="$RANLIB" --strip="$STRIP" --enable-static --disable-shared \
     --disable-doc --disable-programs --disable-debug --disable-stripping --disable-autodetect \
     --enable-gpl --enable-libx265 --enable-libaom --enable-decoder=libaom_av1 --pkg-config-flags=--static \
     --enable-mediacodec --enable-jni --enable-avcodec --enable-avformat --enable-avutil \
     --enable-swscale --enable-swresample --enable-avfilter --enable-avdevice --enable-zlib \
     --enable-asm --enable-inline-asm --enable-neon --disable-vulkan --disable-v4l2-m2m \
     --disable-vaapi --disable-vdpau --optflags='-O2 -fPIC' --extra-ldflags="$LDFLAGS"
   make -j"$JOBS"
   make install)
fi
# Refuse to ship host-architecture objects accidentally picked up by make.
for archive in "$prefix"/lib/*.a; do
  [ -f "$archive" ] || continue
  machines=$("$LLVM_BIN/llvm-readelf.exe" -h "$archive" | grep 'Machine:')
  if [ -z "$machines" ] || printf '%s\n' "$machines" | grep -v 'AArch64' >/dev/null; then
    echo "Wrong or missing architecture in $archive" >&2
    exit 1
  fi
done
# FFmpeg writes absolute libdir/includedir too, so rewriting prefix alone is insufficient.
bash "$(dirname "$patch_dir")/relocate-android-pkgconfig.sh" "$prefix/lib/pkgconfig"
(cd "$prefix" && tar -cf - .) | (cd "$OUTPUT_PREFIX" && tar -xf -)
echo "Installed Android ARM64 dependencies to $OUTPUT_PREFIX"
echo 'Cross-compiled libraries still require final link and device verification.'
