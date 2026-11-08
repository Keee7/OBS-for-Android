#!/usr/bin/env bash
# 生成用于自签名 release APK 的密钥库，并打印 GitHub Secrets 需要的内容。
#
#   bash scripts/generate-release-key.sh                 生成到 ./release-keystore.jks（仓库内，已 gitignore）
#   bash scripts/generate-release-key.sh /secure/obs.jks  生成到指定路径（推荐：仓库外）
#   bash scripts/generate-release-key.sh --print-only     不生成，只把现有密钥打印成 base64
#
# 工具：JDK 自带的 keytool（JAVA_HOME/bin 或 PATH 里要有）。Linux/macOS/Git Bash/WSL 都能跑。
#
# 为什么默认落在仓库里：.gitignore 已有 *.jks / *.keystore 规则，所以即使不小心生成在仓库内
# 也不会被提交。但**推荐把路径指到仓库外** —— 密钥一旦丢进 Git 历史就很难真正清除。
#
# ===== 关于密钥的三个硬道理（写给未来的自己）=====
#   1) 密钥丢了就**永久无法**更新已发布的 APK。Android 只认同一个签名：签名不同的包不能
#      覆盖安装，只能先卸载（用户数据全没）。请立刻离线备份这份 .jks 与它的口令。
#   2) 换密钥 = 换应用身份。发布过的包必须一直用同一把，不要"这次先随便签一下"。
#   3) 口令别写进仓库、别写进 CI 配置明文。GitHub 侧走 Secrets（下面会打印要填的内容）。
set -euo pipefail

print_only=0
if [ "${1:-}" = "--print-only" ]; then
    print_only=1
    shift
fi

# keytool 从 JAVA_HOME 找，找不到再退回 PATH。CI/本地都可能只设了其中一个。
find_keytool() {
    if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/keytool" ]; then
        echo "$JAVA_HOME/bin/keytool"
        return 0
    fi
    if command -v keytool >/dev/null 2>&1; then
        command -v keytool
        return 0
    fi
    return 1
}

KEYTOOL="$(find_keytool)" || {
    echo "FATAL: 找不到 keytool。装一个 JDK（17 即可）并设 JAVA_HOME，或把它加进 PATH。" >&2
    echo "       例如：export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64" >&2
    exit 1
}

DEFAULT_KS="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/release-keystore.jks"
KS="${1:-$DEFAULT_KS}"

# 别名与口令：允许用环境变量覆盖，方便脚本化/CI 复用同一把密钥。
ALIAS="${OBS_RELEASE_KEY_ALIAS:-obs-release}"
STOREPASS="${OBS_RELEASE_STORE_PASSWORD:-}"
KEYPASS="${OBS_RELEASE_KEY_PASSWORD:-}"

print_secrets() {
    local ks="$1"
    [ -f "$ks" ] || {
        echo "FATAL: 找不到密钥库：$ks" >&2
        exit 1
    }
    local b64
    # -w0：禁止 base64 折行。折行会让 GitHub 那边的 secrets 解码出损坏的二进制。
    if base64 --help 2>&1 | grep -q -- '-w'; then
        b64="$(base64 -w0 "$ks")"
    else
        b64="$(base64 "$ks" | tr -d '\n')" # BSD/macOS 的 base64 默认就折行，去掉换行
    fi
    cat <<EOF

============================ 复制到 GitHub Secrets ============================
仓库 → Settings → Secrets and variables → Actions → New repository secret
四项都要建（名字必须一字不差；workflow 就是按这些名字读的）：

  ANDROID_KEYSTORE_BASE64      <下面这一整行 base64>
  ANDROID_KEY_ALIAS            $ALIAS
  ANDROID_KEYSTORE_PASSWORD    <你设的 store 口令>
  ANDROID_KEY_PASSWORD         <你设的 key 口令>

---- ANDROID_KEYSTORE_BASE64 的值（单行，很长，整行复制）----
$b64
---- 结束 ----

本地留档：$ks
另请把密钥与口令离线备份一份；这把丢了就无法再更新已发布的 APK。
==============================================================================
EOF
}

if [ "$print_only" = 1 ] || [ -f "$KS" ]; then
    if [ "$print_only" = 1 ]; then
        print_secrets "$KS"
        exit 0
    fi
    echo "== 密钥库已存在，不覆盖：$KS"
    echo "   想重新生成请先删掉它（旧包将无法用新密钥更新）。"
    print_secrets "$KS"
    exit 0
fi

# 口令没给就自动生成一个强随机口令。自动生成的口令**必须记下来**，
# 所以下面会把它们显示出来（而不是静默写进文件）。
if [ -z "$STOREPASS" ]; then
    STOREPASS="$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    echo "== 未提供 OBS_RELEASE_STORE_PASSWORD，已自动生成一个（见下方输出，请务必留存）"
fi
if [ -z "$KEYPASS" ]; then
    # 默认让 key 口令与 store 口令相同：apksigner/keytool 都接受，
    # 而且少一个要记的东西。想分开就设 OBS_RELEASE_KEY_PASSWORD。
    KEYPASS="$STOREPASS"
fi

[ -d "$(dirname "$KS")" ] || mkdir -p "$(dirname "$KS")"

echo "== 生成密钥库：$KS"
echo "   alias=$ALIAS"
"$KEYTOOL" -genkeypair \
    -keystore "$KS" \
    -alias "$ALIAS" \
    -keyalg RSA -keysize 4096 -validity 10000 \
    -storetype PKCS12 \
    -storepass "$STOREPASS" \
    -keypass "$KEYPASS" \
    -dname "CN=OBS for Android, OU=Self-signed, O=OBS for Android, L=Unknown, ST=Unknown, C=XX"

# 权限收紧：私钥文件不该是全局可读。
chmod 600 "$KS" 2>/dev/null || true

echo ""
echo "生成完成。请立刻做两件事："
echo "  1) 记下别名与口令（下面会显示，只显示这一次）"
echo "  2) 把 .jks 文件离线备份"
echo ""
echo "  ANDROID_KEY_ALIAS=$ALIAS"
echo "  ANDROID_KEYSTORE_PASSWORD=$STOREPASS"
echo "  ANDROID_KEY_PASSWORD=$KEYPASS"

# 自检：能读出来才算生成成功（口令打错/文件损坏在这里就会暴露，
# 而不是等到 CI 跑到最后一步签名时才失败）。
echo ""
echo "== 自检：读一遍刚生成的密钥库"
"$KEYTOOL" -list -keystore "$KS" -storepass "$STOREPASS" -alias "$ALIAS" | head -5

print_secrets "$KS"
