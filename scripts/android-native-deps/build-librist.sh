#!/usr/bin/env bash
# Build librist against the Android cJSON and Mbed TLS already in the prefix.
set -euo pipefail

source_dir=$1
prefix=$2
llvm_bin=$3
target=$4
jobs=$5

case "$target" in
  aarch64-linux-android*) cpu=aarch64 ;;
  x86_64-linux-android*) cpu=x86_64 ;;
  *) echo "Unsupported librist target: $target" >&2; exit 1 ;;
esac

native_path() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -am "$1"
  else
    printf '%s\n' "$1"
  fi
}

exe=
if command -v cygpath >/dev/null 2>&1; then
  exe=.exe
fi
pkg_config=$(command -v pkg-config)
build_dir="$source_dir/.obs-android-build"
mkdir -p "$build_dir"
cross_file="$build_dir/obs-cross.ini"
cat > "$cross_file" <<EOF
[binaries]
c = ['$(native_path "$llvm_bin/clang$exe")', '--target=$target']
ar = '$(native_path "$llvm_bin/llvm-ar$exe")'
strip = '$(native_path "$llvm_bin/llvm-strip$exe")'
pkg-config = '$(native_path "$pkg_config")'

[host_machine]
system = 'android'
cpu_family = '$cpu'
cpu = '$cpu'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$(native_path "$prefix/lib/pkgconfig")'
EOF

meson_command=(meson)
if ! command -v meson >/dev/null 2>&1; then
  meson_command=(python -m mesonbuild.mesonmain)
fi

setup=(setup --cross-file "$(native_path "$cross_file")")
if [ -f "$build_dir/meson-private/coredata.dat" ]; then
  setup+=(--reconfigure)
fi
"${meson_command[@]}" "${setup[@]}" "$(native_path "$build_dir")" "$(native_path "$source_dir")" \
  --prefix "$(native_path "$prefix")" --libdir lib --buildtype release \
  --default-library static --wrap-mode nodownload -Dtest=false -Dbuilt_tools=false -Dfallback_builtin=false
"${meson_command[@]}" compile -C "$(native_path "$build_dir")" -j "$jobs"
"${meson_command[@]}" install -C "$(native_path "$build_dir")" --no-rebuild
