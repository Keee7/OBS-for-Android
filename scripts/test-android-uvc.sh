#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
source_dir="$repo/obs-studio/plugins/android-capture"
output=$(mktemp -d "${TMPDIR:-/tmp}/obs-uvc-test.XXXXXX")
compiler=${CC:-cc}
for name in uvc-bulk-pump uvc-raw-payload uvc-cadence-stats uvc-stream-buffer uvc-recovery; do
  args=(-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -I"$source_dir")
  if [ "$name" = uvc-bulk-pump ]; then
    args+=(-pthread -I"$source_dir/tests/mock" "$source_dir/uvc-bulk-pump.c")
  fi
  "$compiler" "${args[@]}" "$source_dir/tests/$name-test.c" -o "$output/$name-test"
  "$output/$name-test"
done
python3 "$repo/scripts/test-uvc-policy.py" --compiler "$compiler"
python3 "$source_dir/tests/test-uvc-negotiation.py" --compiler "$compiler"
echo 'PASS: UVC host regressions (hardware throughput/CPU still require device measurements).'
