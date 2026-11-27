# CI：自动构建 APK / 签名 / 发布

用 GitHub Actions 自动构建 arm64-v8a 的 APK，并用你自己的密钥签成 release 包。

**文件**
| 文件 | 作用 |
| --- | --- |
| `.github/workflows/build-apk.yml` | GitHub Actions 工作流（触发、缓存、artifact、Release） |
| `scripts/ci-build-android.sh` | 校验已安装的工具链，再编译依赖和 OBS、打包及签名 |
| `scripts/generate-release-key.sh` | 生成签名密钥库，并打印要填进 Secrets 的内容 |

---

## 一、第一次用：配密钥（约 2 分钟）

### 1. 本地生成密钥

```sh
# 生成到仓库外更稳妥（默认落在仓库内的 release-keystore.jks，该后缀已被 .gitignore 排除）
bash scripts/generate-release-key.sh ~/obs-release.jks
```

脚本会打印四样东西，记下来：别名、store 口令、key 口令，以及**一整行 base64**。

> 只有 JDK、没有图形界面也能跑（纯 `keytool`）。Windows 上用 Git Bash 或 WSL 执行同样脚本。

### 2. 填进 GitHub Secrets

仓库 → **Settings → Secrets and variables → Actions → New repository secret**，
建这四项（名字必须一字不差，workflow 就是按这些名字读的）：

| Secret 名 | 值 |
| --- | --- |
| `ANDROID_KEYSTORE_BASE64` | 脚本打印的那一整行 base64 |
| `ANDROID_KEY_ALIAS` | `obs-release`（或用 `OBS_RELEASE_KEY_ALIAS` 指定的别名） |
| `ANDROID_KEYSTORE_PASSWORD` | 脚本打印的 store 口令 |
| `ANDROID_KEY_PASSWORD` | 脚本打印的 key 口令 |

### 3. 关于密钥的三件必须知道的事

1. **这把丢了就永久无法更新已发布的 APK。** Android 只认同一签名：签名不同的包不能覆盖安装，
   只能先卸载（用户数据全没）。请把 `.jks` 与口令**离线备份**一份。
2. **不要提交进仓库。** `.gitignore` 已排除 `*.jks` / `*.keystore` / `*.p12` / `*.pfx`，
   但被 Git 记住过的密钥很难真正清除 —— 别依赖这条规则，直接放在仓库外。
3. **这把是"自签名"密钥**，不是任何官方身份。它只证明"后续版本都出自同一把密钥"，
   不证明包本身可信。仓库里的 `scripts/signing/debug.keystore` 是公开开发密钥，
   **不要**拿它当 release 密钥。

---

## 二、触发构建（节省 CI 配额）

| 方式 | 结果 |
| --- | --- |
| push 到 `main`（commit 含 `[build-apk]`） | 构建；配了 Secrets 就签名（普通提交跳过，不消耗分钟数） |
| push 一个 `v*` tag（如 `v0.1.19`） | 构建 + 签名 + **创建 GitHub Release** 并附件 APK（跳过重复 Artifact 上传） |
| Pull Request（非 Draft） | 使用仓库中的公开开发密钥签名，产物可安装；与私有密钥签名的包不能直接覆盖安装 |
| 每周一凌晨 `schedule`（`0 3 * * 1` UTC） | 自动刷新 `main` 上的 `obsdeps` / `obsqt` / `ccache` / Gradle 基线缓存，防止 7 天过期与 PR 冷编 |
| 手动 `workflow_dispatch` | 可勾选"签名"与"发布 Release" |

产物在 Actions 页面的 **Artifacts** 里（默认保留 3 天，Release 触发不额外占 Artifact 配额），文件名 `obs-android-apk-<commit sha>`。
每次构建的 SHA-256 与是否签名会写进该次运行的 **Summary**。

打 tag 发版：

```sh
git tag v0.1.19
git push origin v0.1.19
```

日常提交如确需直接构建 APK，在 commit message 中带上 `[build-apk]`：

```sh
git commit -m "fix(uvc): ... [build-apk]"
```

---

## 三、构建耗时与缓存

仓库**不含**任何依赖二进制（`deps-android/prebuilt/`、`frontend/cmake/android/libs/`
都被 `.gitignore` 排除），所以 CI 从源码构建全部 14 个依赖，FFmpeg 与 x264 是大头。

| 场景 | 大致耗时 |
| --- | --- |
| 首次（缓存全空） | **35–50 分钟** |
| 依赖 + 工具链 + ccache 命中 | **6–10 分钟** |

CI 复用 runner 上的 Android SDK、NDK 和 CMake，使用 JDK 21（工具链版本号统一定义在 `scripts/toolchain-versions.env`），通过 `setup-gradle` 安装 Gradle 9.5.1（开启 `--parallel`），OBS 主构建使用 `-O2 -g0 -DNDEBUG` 并关闭 IPO（最终 `.so` 都会被 strip，免去无谓的调试符号生成以提速并压小缓存）。缓存如下：

| 缓存 | 内容 | 失效条件 / 策略 |
| --- | --- | --- |
| `obsdeps-v3-…` | `deps-android/prebuilt`（仅预编产物，不含 `src/`） | `deps-android/*.sh` / `scripts/android-native-deps/*` / `relocate-android-pkgconfig.sh` 内容变了 |
| `obsqt-v3-…` | `~/Qt`（Qt 宿主、Android arm64 套件与 `qtbase` 源码包） | Qt 版本变了 |
| `obsccache-v2-…` | `~/.ccache`（仅缓存 OBS 主构建，上限 512M；PR 只读不回写） | 首选当前提交和构建次数，按 NDK 与 Qt 版本回退 |
| `setup-gradle` | Gradle 用户目录中的缓存 | 由 `gradle/actions/setup-gradle` 管理，PR 只读 |

> GitHub 的 Actions 缓存**约 7 天不用会被回收**，且仓库总量有配额。每周一 `schedule` 会自动刷新 `main` 上的基线缓存。想强制作废：改 `deps-android/versions.sh` 或删缓存的 key 前缀。

---

## 四、本地复现（和 CI 同样的路径）

在 Linux 或 WSL 上，先按 [BUILDING.md 的工具链安装步骤](../../BUILDING.md#ci-toolchain-setup) 准备环境，再在仓库根目录运行。

```sh
# 只跑 CMake configure，快速验证 CMake 改动 —— 几分钟
bash scripts/ci-build-android.sh configure

# 编译依赖和 OBS，然后打包 APK
bash scripts/ci-build-android.sh
```

本地想同时签名，就设好四个 `SIGN_*` 变量：

```sh
SIGN_KEYSTORE=~/obs-release.jks \
SIGN_KEY_ALIAS=obs-release \
SIGN_STORE_PASS='<store 口令>' \
SIGN_KEY_PASS='<key 口令>' \
bash scripts/ci-build-android.sh
```

产物：`build-fe-arm64-v8a-ci/OBS-Android-arm64-v8a-release.apk`（已签名）
或该目录下的 `*.apk`（未签名）。

### 可覆盖的环境变量

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `NDK_VERSION` | `r30` | 与仓库 Windows 构建一致 |
| `QT_VERSION` | `6.12.0` | 由 aqt 按版本解析官方仓库 |
| `ANDROID_SDK_ROOT` | 自动查找 | 指向已安装的 SDK，脚本也检查 `ANDROID_HOME` 和 runner 的 SDK 目录 |
| `GRADLE_VERSION` | `9.5.1`（来自 `toolchain-versions.env`） | 校验 `PATH` 中的 Gradle 版本；传 `system` 可放宽为 `>= 9.0` |
| `GRADLE_BIN` | `PATH` 中的 `gradle` | 可显式指定 Gradle 可执行文件的完整路径 |
| `BUILD_TOOLS` | `36.0.0` | 也用于 zipalign / apksigner |
| `PLATFORM` | `android-36` | Qt 6.12.0 的 Gradle 模板要 platform 36 |
| `JOBS` | `nproc` | |
| `QT_ROOT` | `$HOME/Qt` | Qt 安装根目录，包含版本子目录 |
| `DL_ROOT` | 仓库同级的 `obs-dl` | 下载物落盘（**不**缓存） |

---

## 五、已知限制

- CI 构建 APK 并校验包名、版本、原生库和插件。已签名的 APK 还会校验签名和 16 KB 对齐。
- **只有 arm64-v8a**。与本仓其余构建路径一致（x86_64 需要改 ABI 与依赖构建参数）。
- **Qt 由 `aqt install-qt` 安装**（`all_os/android` + `linux/desktop`），不需要 Qt 账号。
  aqt 按 `QT_VERSION` 选择官方包。升级时需同步更新 Android 运行时补丁、校验值和 Gradle。
- **依赖构建是源码级**：`fetch.sh` 里 `x264`/`rnnoise` 用的是分支（`master`）而不是 tag，
  所以不同时间构建出来的依赖**不是逐字节可复现**的。要严格复现请把版本钉到具体 commit。
- **Release 只在该次构建真的签过名时才创建**（避免发出装不上的包）。
- 工作流对 PR 使用公开开发密钥签名，不使用私有 Secrets，也不发布 Release。来自 fork 的 PR 拿不到 Secrets，这是 GitHub 的设计。

---

## 六、排错

| 现象 | 原因 / 处理 |
| --- | --- |
| `找不到 7-Zip 解包器` | Ubuntu 24.04 要装 `7zip`（`p7zip-full` 已变 transitional 包，不提供 `7z`） |
| `autoreconf: not found` | 装 `autoconf automake libtool` |
| `CMake 版本过低` | 安装 CMake 3.31.6，并确保在 `PATH` 中能找到它。脚本检查的最低版本为 3.28 |
| `Unrecognized named-value: 'runner'` | job 级 `env` 里用了 `runner.*`。`runner` 上下文只在 step 级可用；job 级改用 `github.*` |
| `Could not find ... nlohmann_json` | 前端 `CMakeLists.txt:73` 无条件需要它，现已纳入 `deps-android` 清单（`defs.sh` 的 `build_nlohmann_json`）并由 `verify_deps_complete()` 校验；若报错可清依赖缓存重建 |
| `uic: error while loading shared libraries: libicui18n.so.73` / `AutoUic subprocess error` / exit 127 | **旧版 6.9.3 手工解包**漏了独立的 ICU 73 运行时包（Ubuntu 24.04 自带 74）。现在用 aqt 装完整 Qt 6.12.0 宿主包，不再缺 ICU；若换回 6.9.x 手工解包才需要重新处理 |
| `Host tool ... 跑不起来` / 工具自检 FATAL | 宿主 Qt 工具缺共享库。脚本的 `probe_host_tools()` 在装配阶段就会撞出来（秒级），不必等构建 |
| 签名步骤报口令错 | Secrets 里的四项要来自**同一次** `generate-release-key.sh` 输出 |
| `base64: invalid input` | `ANDROID_KEYSTORE_BASE64` 粘贴时断行/带了多余字符；重新整行复制 |
| 构建成功但 APK 没插件 | 看日志里"已暂存"那一行；`package_apk` 缺插件会直接 FATAL，不会静默出空壳 |
| 依赖构建每次都重来 | 缓存被回收了（约 7 天不用）。属预期，等一次"首次"耗时 |

### 关于"工具链缺件"这类失败

CI 在这里失败过三次，都是**脚本装的东西少了**，而不是 OBS 源码的问题：

| 次数 | 缺什么 | 为什么本地没暴露 |
| --- | --- | --- |
| 1 | `runner` 上下文用错位置 | YAML 语法能过，PyYAML 查不出；后来用 actionlint 才拦下 |
| 2 | `nlohmann_json` | 本机**恰好**装了 conda 的版本（`CMakeCache` 里指向 `miniconda3`） |
| 3 | Qt 宿主要的 ICU 73 | 仓库的 Windows 构建用 `msvc2022_64` 宿主，Linux 宿主是 CI 才第一次真正跑起来 |

共同点：**本地"能过"不代表 CI 能过，因为本地工具链带着机器上的偶发状态。**
因此脚本里加了装配阶段的 `probe_host_tools()` 自检——宿主工具跑不起来就在这里失败（秒级），
而不是跑完几十分钟的依赖构建后才在 `AutoUic` 报 exit 127。

改 CI 工具链相关的东西时，请跑 `actionlint .github/workflows/*.yml`，并优先信 CI 的实际日志。

