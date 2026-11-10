#!/usr/bin/env bash
# Relocate installed pkg-config metadata after copying an Android prefix.
# Preserve package-specific libdir/includedir values except FFmpeg's generated
# absolute paths, which must follow the relocated prefix too.
set -euo pipefail
pc_dir=${1:?Pass the installed lib/pkgconfig directory}
[ -d "$pc_dir" ] || exit 0
for pc in "$pc_dir"/*.pc; do
  [ -f "$pc" ] || continue
  sed -i 's|^prefix=.*|prefix=${pcfiledir}/../..|' "$pc"
  case "$(basename "$pc")" in
    libav*.pc|libsw*.pc|libpostproc.pc)
      # FFmpeg may also embed its build prefix in Libs/Cflags. On MSYS,
      # pkg-config can spell the same path as C:/... instead of /c/....
      old_libdir=$(sed -n 's|^libdir=||p' "$pc")
      old_includedir=$(sed -n 's|^includedir=||p' "$pc")
      native_libdir=$old_libdir
      native_includedir=$old_includedir
      if command -v cygpath >/dev/null 2>&1; then
        case "$old_libdir" in /*) native_libdir=$(cygpath -m "$old_libdir");; esac
        case "$old_includedir" in /*) native_includedir=$(cygpath -m "$old_includedir");; esac
      fi
      awk -v lib="$old_libdir" -v nlib="$native_libdir" \
          -v inc="$old_includedir" -v ninc="$native_includedir" '
        function replace_literal(s, old, new, pos, out) {
          if (old == "" || index(old, "${") != 0) return s
          out = ""
          while ((pos = index(s, old)) != 0) {
            out = out substr(s, 1, pos - 1) new
            s = substr(s, pos + length(old))
          }
          return out s
        }
        /^(Libs|Libs[.]private|Cflags):/ {
          $0 = replace_literal($0, lib, "${libdir}")
          $0 = replace_literal($0, nlib, "${libdir}")
          $0 = replace_literal($0, inc, "${includedir}")
          $0 = replace_literal($0, ninc, "${includedir}")
        }
        { print }
      ' "$pc" > "$pc.relocate"
      mv "$pc.relocate" "$pc"
      sed -i -e 's|^libdir=.*|libdir=${prefix}/lib|' \
             -e 's|^includedir=.*|includedir=${prefix}/include|' "$pc"
      ;;
  esac
done
