#!/bin/bash
# 下载依赖源码包到 deps-android/src/
# 运行环境：Bash + curl（WSL/Linux 或 Windows Git Bash）。
#
#   ./fetch.sh                 下载全部批次
#   ./fetch.sh core            下载某个批次
#   ./fetch.sh jansson ffmpeg  下载指定依赖
#
# 每个归档下载后会做 tar 全量列举（完整性）校验，失败不计入成功。
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=versions.sh
source "$HERE/versions.sh"

PROXY="${OBS_PROXY:-}"
SRC="$HERE/src"
mkdir -p "$SRC"

# 完整性校验：把归档内容**全量列举**一遍。能同时挡住两类事故：
#   * 下载被截断（-C - 断点续传 / 网络抖动）—— tar 读到最后会报 unexpected EOF；
#   * 把 HTML 错误页、代理返回页当成源码包存下来 —— tar 认不出压缩格式。
_archive_list_flags() {
  case "$1" in
    *.tar.gz|*.tgz)   echo "-tzf" ;;
    *.tar.bz2|*.tbz2) echo "-tjf" ;;
    *.tar.xz)         echo "-tJf" ;;
    *)                echo "-tf" ;;   # 未知格式：交给 tar 按魔数自动识别
  esac
}

verify_archive() {
  local f=$1
  local flags
  flags=$(_archive_list_flags "$f")
  tar "$flags" "$f" >/dev/null 2>&1
}

# 解析参数：批次名 或 依赖名
TARGETS=""
for a in "$@"; do
  if echo " $DEP_BATCHES " | grep -q " $a "; then
    TARGETS="$TARGETS $(batch_deps "$a")"
  else
    TARGETS="$TARGETS $a"
  fi
done
[ -z "${TARGETS// /}" ] && TARGETS=$(dep_names | tr '\n' ' ')

FAIL=""
SKIP=""
for d in $TARGETS; do
  url=$(dep_url "$d")
  if [ -z "$url" ]; then
    echo "[SKIP] $d : 未配置下载地址"
    SKIP="$SKIP $d"; continue
  fi
  ver=$(dep_version "$d")
  fn="$(basename "$url")"
  out="$SRC/${d}-${ver}-${fn}"
  if [ -s "$out" ]; then
    echo "[CACHE] $d ($ver) 已存在"
    continue
  fi
  curl_args=(-fL --retry 3 --retry-delay 2 -C - -sS)
  case "$url" in
    *github.com*) [ -z "$PROXY" ] || curl_args+=(-x "$PROXY") ;;
  esac
  echo "[GET ] $d $ver -> $fn"
  if ! curl "${curl_args[@]}" -o "$out.part" "$url"; then
    echo "[FAIL] $d 下载失败：$url"
    rm -f "$out.part"; FAIL="$FAIL $d"; continue
  fi
  mv "$out.part" "$out"
  if ! verify_archive "$out"; then
    echo "[FAIL] $d 归档完整性校验失败（tar 全量列举失败，可能是截断包或 HTML 错误页）"
    rm -f "$out"; FAIL="$FAIL $d"; continue
  fi
  printf '       %8d bytes\n' "$(stat -c %s "$out")"
done

echo "=============================="
echo "完成。失败:${FAIL:- 无}  跳过:${SKIP:- 无}"
[ -n "$FAIL" ] && exit 1
exit 0
