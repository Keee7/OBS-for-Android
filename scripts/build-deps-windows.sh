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
LLVM_BIN=${LLVM_BIN:-/clang64/bin}
case "$LLVM_BIN" in *' '*) echo 'LLVM_BIN must not contain spaces; use a Windows short path' >&2; exit 1;; esac
JOBS=${JOBS:-4}
API=${API:-29}
for dep in bash make tar sed cp cygpath patch; do command -v "$dep" >/dev/null || { echo "Missing tool: $dep" >&2; exit 1; }; done
for exe in clang.exe llvm-ar.exe llvm-ranlib.exe llvm-strip.exe llvm-nm.exe llvm-strings.exe llvm-readelf.exe; do
  test -f "$LLVM_BIN/$exe" || { echo "Missing LLVM tool: $LLVM_BIN/$exe" >&2; exit 1; }
done
test -f "$SYSROOT/usr/lib/aarch64-linux-android/$API/libc.so" || { echo "Incomplete NDK sysroot: missing API $API libc.so" >&2; exit 1; }
test -f "$SOURCES_ROOT/x264/configure" || { echo 'Missing official x264 source' >&2; exit 1; }
test -f "$SOURCES_ROOT/ffmpeg-7.1.3/configure" || { echo 'Missing official FFmpeg 7.1.3 source' >&2; exit 1; }
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
export CFLAGS='-O2 -fPIC' LDFLAGS='-Wl,-z,max-page-size=16384'
prepare() {
  local source="$1" dest="$2"
  if [ ! -f "$dest/configure" ]; then
    mkdir -p "$dest"
    (cd "$source" && tar --exclude=.git -cf - .) | (cd "$dest" && tar -xf -)
  fi
}
prepare "$SOURCES_ROOT/x264" "$WORK_ROOT/src/x264"
prepare "$SOURCES_ROOT/ffmpeg-7.1.3" "$WORK_ROOT/src/ffmpeg"
# Idempotently apply our MediaCodec fixes to a pristine or existing source tree.
patch_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/android-native-deps"
for patch_file in "$patch_dir/ffmpeg-7.1.3-mediacodec-codec-name.patch" "$patch_dir/ffmpeg-7.1.3-mediacodec-crop.patch"; do
  if patch -d "$WORK_ROOT/src/ffmpeg" -p1 -R --force --dry-run --silent < "$patch_file"; then
    echo "Already applied: $patch_file"
  elif patch -d "$WORK_ROOT/src/ffmpeg" -p1 --force --dry-run --silent < "$patch_file"; then
    patch -d "$WORK_ROOT/src/ffmpeg" -p1 --force < "$patch_file"
  else
    echo "FFmpeg patch does not match this source tree: $patch_file" >&2
    exit 1
  fi
done
prefix="$WORK_ROOT/prefix"
targets=" ${*:-x264 ffmpeg} "
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
  (cd "$WORK_ROOT/src/ffmpeg"
   ./configure --prefix="$prefix" --enable-cross-compile --target-os=android --arch=aarch64 \
     --cc="$CC" --ar="$AR" --ranlib="$RANLIB" --strip="$STRIP" --enable-static --disable-shared \
     --disable-doc --disable-programs --disable-debug --disable-stripping --disable-autodetect \
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
# pkg-config files must remain usable after relocation to the requested prefix.
for pc in "$prefix"/lib/pkgconfig/*.pc; do
  [ -f "$pc" ] || continue
  sed -i 's|^prefix=.*|prefix=${pcfiledir}/../..|' "$pc"
done
(cd "$prefix" && tar -cf - .) | (cd "$OUTPUT_PREFIX" && tar -xf -)
echo "Installed Android ARM64 dependencies to $OUTPUT_PREFIX"
echo 'Cross-compiled libraries still require final link and device verification.'
