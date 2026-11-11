#!/bin/bash
# Android 依赖交叉编译（在 WSL2 Ubuntu 内运行）
#
#   ./build.sh --list                       查看依赖与批次
#   ./build.sh arm64-v8a                    构建全部依赖
#   ./build.sh arm64-v8a core               构建某个批次
#   ./build.sh arm64-v8a jansson ffmpeg     构建指定依赖
#   ./build.sh arm64-v8a core --clean       强制重做
#
# 产物：<此目录>/prebuilt/<abi>/{include,lib,...}
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/versions.sh"
source "$HERE/defs.sh"

NDK="${NDK:-$HOME/ndk/30.0.16138531}"
API="${API:-29}"
JOBS="${JOBS:-$(nproc)}"
WORK="${WORK:-$HOME/deps-work}"
HOST_TAG=linux-x86_64
BUILD_TRIPLE="$(uname -m)-unknown-linux-gnu"

export NDK API JOBS BUILD_TRIPLE

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

case "${1:-}" in
  ""|--list)
    if [ "${1:-}" = "--list" ]; then
      echo "NDK=$NDK  API=$API  JOBS=$JOBS"
      for b in $DEP_BATCHES; do
        echo "[批次 $b]"
        for d in $(batch_deps "$b"); do
          printf '   %-10s %-10s\n' "$d" "$(dep_version "$d")"
        done
      done
      exit 0
    fi
    usage ;;
esac

ABI=$1; shift
case "$ABI" in
  arm64-v8a) TRIPLE=aarch64-linux-android; ARCH_EXPECT='AArch64' ;;
  x86_64)    TRIPLE=x86_64-linux-android;  ARCH_EXPECT='Advanced Micro Devices X86-64' ;;
  armeabi-v7a) TRIPLE=arm-linux-androideabi; ARCH_EXPECT='ARM' ;;
  *) echo "不支持的 ABI: $ABI (arm64-v8a | x86_64)" >&2; exit 1 ;;
esac

TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
SYSROOT="$TOOLCHAIN/sysroot"
[ -d "$SYSROOT" ] || { echo "FATAL: NDK sysroot 不存在：$SYSROOT" >&2; exit 1; }

export ABI TRIPLE TOOLCHAIN SYSROOT
# ccache：把 CC/CXX 包一层，让读 $CC 的依赖（autotools 系 / x264 / ffmpeg）也进缓存。
# 生成与真实编译器同名的 wrapper，而不是 "ccache <abs>" —— configure 会对 $CC
# 做 --version / test -x 等探测，带空格的字符串容易踩坑；CCACHE_* 由 ci-build-android.sh
# 统一配置，主构建用 CMAKE_*_COMPILER_LAUNCHER，共用一份缓存。
CC_REAL="$TOOLCHAIN/bin/${TRIPLE}${API}-clang"
CXX_REAL="$TOOLCHAIN/bin/${TRIPLE}${API}-clang++"
if [ "${USE_CCACHE:-0}" = "1" ] && command -v ccache >/dev/null 2>&1; then
  SHIM="$WORK/ccache-shim"
  mkdir -p "$SHIM"
  CCACHE_BIN="$(command -v ccache)"
  printf '#!/bin/sh\nexec "%s" "%s" "$@"\n' "$CCACHE_BIN" "$CC_REAL" > "$SHIM/${TRIPLE}${API}-clang"
  printf '#!/bin/sh\nexec "%s" "%s" "$@"\n' "$CCACHE_BIN" "$CXX_REAL" > "$SHIM/${TRIPLE}${API}-clang++"
  chmod +x "$SHIM/${TRIPLE}${API}-clang" "$SHIM/${TRIPLE}${API}-clang++"
  CC="$SHIM/${TRIPLE}${API}-clang"
  CXX="$SHIM/${TRIPLE}${API}-clang++"
else
  CC="$CC_REAL"
  CXX="$CXX_REAL"
fi
export CC CXX
export AR="$TOOLCHAIN/bin/llvm-ar"
export AS=$CC
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
export STRIP="$TOOLCHAIN/bin/llvm-strip"
export NM="$TOOLCHAIN/bin/llvm-nm"
export OBJCOPY="$TOOLCHAIN/bin/llvm-objcopy"
export OBJDUMP="$TOOLCHAIN/bin/llvm-objdump"
export LD="$TOOLCHAIN/bin/ld.lld"
# NDK 30 无 ${TRIPLE}-strings，x264 的 endian test 会因找不到 strings 而失败
export STRINGS="$TOOLCHAIN/bin/llvm-strings"

PREFIX="$WORK/prefix/$ABI"
STATE="$WORK/state/$ABI"
EXPORT="$HERE/prebuilt/$ABI"
mkdir -p "$PREFIX" "$STATE" "$WORK/src" "$EXPORT"
export PREFIX

# 解析依赖列表
CLEAN=0
TARGETS=""
for a in "$@"; do
  case "$a" in
    --clean) CLEAN=1 ;;
    *) if echo " $DEP_BATCHES " | grep -q " $a "; then
         TARGETS="$TARGETS $(batch_deps "$a")"
       else TARGETS="$TARGETS $a"; fi ;;
  esac
done
[ -z "${TARGETS// /}" ] && for b in $DEP_BATCHES; do TARGETS="$TARGETS $(batch_deps "$b")"; done

# A changed patch needs pristine sources and must invalidate the shared prefix's stamp.
ffmpeg_patch="$HERE/../scripts/android-native-deps/ffmpeg-8.0.3-mediacodec-android.patch"
ffmpeg_patch_digest=$(sha256sum < "$ffmpeg_patch") || exit 1
ffmpeg_source_id="ffmpeg-$(dep_version ffmpeg)-${ffmpeg_patch_digest%% *}"

codec_defs_digest=$(sha256sum < "$HERE/defs.sh") || exit 1

src_dir() {
  local name=$1
  if [ "$name" = ffmpeg ]; then name=$ffmpeg_source_id; fi
  echo "$WORK/src/$ABI/$name"
}

_extract() {
  local dep=$1
  local fn
  fn=$(ls "$HERE"/src/"$dep-$(dep_version "$dep")-"* 2>/dev/null | head -1)
  [ -n "$fn" ] || { echo "   ! 缺少源码包：src/$dep-*（先运行 fetch.sh）"; return 1; }
  local d; d=$(src_dir "$dep")
  if [ -d "$d" ] && [ -n "$(ls -A "$d" 2>/dev/null)" ]; then
    return 0
  fi
  echo "   解压 $(basename "$fn")"
  rm -rf "$d"; mkdir -p "$d"
  tar -xf "$fn" -C "$d" --strip-components=1 || { echo "   ! 解压失败"; return 1; }
}

export_to_windows() {
  echo "   -> 同步到 $EXPORT"
  mkdir -p "$EXPORT"
  ( cd "$PREFIX" && tar -cf - . ) | ( cd "$EXPORT" && tar -xf - ) \
    || echo "   ! 同步失败（Windows 侧 prebuilt 可能不完整）"
  _relocate_cmake_package "$EXPORT"
  bash "$HERE/../scripts/relocate-android-pkgconfig.sh" "$EXPORT/lib/pkgconfig"
}

# 依赖是装在 $PREFIX（Linux 路径）下的，CMake 导出的 *Targets.cmake 会把某些
# 绝对路径原样写进去 —— 实测 libCURL 的 INTERFACE_LINK_LIBRARIES 里就是
# /home/bbpcs/deps-work/prefix/<abi>/lib/libmbedtls.a。WSL 里 configure 能解析，
# Windows 侧 configure 会把它当成 ninja 依赖直接报 missing and no known rule to make it。
# *Targets.cmake 自己定义了 _IMPORT_PREFIX（由本文件位置回溯三级得到包根目录），
# 换成它之后同一份包在两个宿主上都能用。
_relocate_cmake_package() {
  local root="$1" n
  [ -d "$root/lib/cmake" ] || return 0
  n=$(grep -rl -- "$PREFIX" "$root/lib/cmake" "$root/share/cmake" 2>/dev/null | wc -l)
  [ "$n" = 0 ] && return 0
  grep -rl -- "$PREFIX" "$root/lib/cmake" "$root/share/cmake" 2>/dev/null |
    while read -r f; do
      sed -i "s|$PREFIX|\${_IMPORT_PREFIX}|g" "$f"
      echo "   ~ 已重定位 $f"
    done
}

# 交叉编译常见的坑：源码树被别的 ABI 复用，陈旧 .o 直接进包，而构建仍然报成功。
# 因此每个依赖构建完都要核对 $PREFIX 下所有静态库的目标架构。
_audit_arch() {
  local f m bad=0
  for f in "$PREFIX"/lib/*.a "$PREFIX"/lib/*/*.a; do
    [ -f "$f" ] || continue
    m=$(readelf -h "$f" 2>/dev/null |
        sed -n 's/^[[:space:]]*Machine:[[:space:]]*\(.*\)[[:space:]]*$/\1/p' | sort -u)
    if [ "$m" != "$ARCH_EXPECT" ]; then
      echo "   ! 架构不符：$(basename "$f") => [$m]"
      bad=1
    fi
  done
  return $bad
}

FAILED=""
BUILT=0
for dep in $TARGETS; do
  fn="build_$(echo "$dep" | tr '-' '_')"
  if ! declare -f "$fn" >/dev/null; then
    echo "[$dep] 无构建定义（函数 $fn 未定义），跳过"; FAILED="$FAILED $dep"; continue
  fi
  stamp="$STATE/$dep.done"
  stamp_id=""
  case "$dep" in
    aom) stamp_id="aom-$(dep_version aom)-${codec_defs_digest%% *}" ;;
    ffmpeg) stamp_id="$ffmpeg_source_id-${codec_defs_digest%% *}" ;;
  esac
  if [ "$CLEAN" = 0 ] && [ -f "$stamp" ] &&
     { [ -z "$stamp_id" ] || [ "$(cat "$stamp")" = "$stamp_id" ]; }; then
    echo "[$dep] 已完成，跳过（--clean 可重做）"
    continue
  fi
  echo "==================================================="
  echo "[$dep $(dep_version "$dep")] ABI=$ABI 开始构建"
  _extract "$dep" || { FAILED="$FAILED $dep"; continue; }
  start=$(date +%s)
  # 子 shell 放在 if 条件里时，bash 会让内部所有命令都忽略 -e（即使里面再 set -e），
  # 于是失败会被后面的 echo 掩盖成"成功"并打上永久 stamp。改成普通上下文执行、
  # 显式取返回码，才能保持"失败记入 FAILED、成功才落 stamp"的原语义。
  ( set -e; "$fn" ); rc=$?
  if [ "$rc" -eq 0 ]; then
    if ! _audit_arch; then
      echo "[$dep] 架构校验失败（$ABI 的产物里混进了别的架构），需 --clean 重做"
      FAILED="$FAILED $dep"
      continue
    fi
    dur=$(( $(date +%s) - start ))
    if [ -n "$stamp_id" ]; then
      printf '%s\n' "$stamp_id" > "$stamp"
    else
      touch "$stamp"
    fi
    echo "[$dep] 成功（${dur}s）"
    BUILT=$((BUILT+1))
    export_to_windows
  else
    echo "[$dep] 失败"
    FAILED="$FAILED $dep"
  fi
done

echo "==================================================="
echo "本次成功构建 $BUILT 个；失败:${FAILED:- 无}"
[ -n "$FAILED" ] && exit 1
exit 0
