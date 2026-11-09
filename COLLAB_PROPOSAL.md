# 协作提案：来自 TTTornado/OBS-For-Android 的工程化改动

本分支（`port/our-engineering`）把 [TTTornado/OBS-For-Android](https://github.com/TTTornado/OBS-For-Android)
里做过的一批**构建与合规**改动，适配到本仓库的布局（`deps-android/` + `obs-studio/` 子目录）。
这些改动与 OBS 版本无关，不需要动 `obs-studio/` 里的功能代码。

## 来源与关系

两条线都源自 cunminF/OBS_for_Android，但**没有共享的 git 历史**：

| | 本仓库 | TTTornado/OBS-For-Android |
|---|---|---|
| 基线 | OBS 32.2.1 (`57bfcf10a`)，保留 cunminF 提交历史 | OBS 33.0.1 (`65c320b`)，从 obsproject/obs-studio fork |
| 布局 | `obs-studio/` 子目录 | 根级布局 |
| CI | `build-apk.yml` + `ci-build-android.sh` | 单 job 三阶段 + ccache 三层注入 |

因此 `git cherry-pick` / `merge` 跨仓库不可行，这里的改动是**按文件手工适配**的，
不是补丁直接套用。所有新增/修改的脚本沿用 GPL-2.0-or-later，保留原有作者与许可声明。

## 本分支包含什么

1. **移除 fdk-aac**（`deps-android/versions.sh`、`defs.sh`）
   fdk-aac 的授权与 GPLv2+ 不兼容，不适合随 APK 分发。本仓库的
   `obs-studio/plugins/obs-libfdk/CMakeLists.txt` 默认 `ENABLE_LIBFDK=OFF` 并
   `target_disable()`，构建脚本也未传 `-DENABLE_LIBFDK=ON`，所以移除依赖不影响构建。
   这是 OBS-For-Android 的 P0 合规修复之一。

2. **下载归档完整性校验**（`deps-android/fetch.sh`）
   每个 tar 包下载后做一次**全量列举**（`tar -t`），挡住两类事故：断点续传/网络抖动
   导致的截断包，以及把 HTML 错误页/代理返回页当成源码包存下来。失败计入 FAIL 并
   删除可疑文件，不留下坏包。

3. **构建健壮性**（`deps-android/build.sh`、`defs.sh`）
   - `if ( set -e; "$fn" ); then` 这种写法里，bash 的条件上下文会让子 shell 内所有命令
     忽略 `-e`，失败被后续 echo 掩盖成"成功"并打上永久 stamp；改成普通上下文执行 +
     显式取返回码。
   - header-only 依赖（simde/uthash）的 `mkdir` / `rm` / `cp` 全部补 `|| return 1`，
     避免 `cp` 失败但最后一句 echo 成功导致的"假成功"。

4. **ccache 三层注入**（他的 CI 原先无编译缓存，timeout 180 分钟）
   - `build.sh`：用与真实编译器**同名**的 wrapper（`exec ccache <real> "$@"`）包住
     `CC`/`CXX`，覆盖 autotools 系 / x264 / FFmpeg；不用 `CC="ccache <abs>"` 是因为
     configure 会对 `$CC` 做 `--version` / `test -x` 探测，带空格容易踩坑。
   - `defs.sh` 的 `_run_cmake` 与 `ci-build-android.sh` 的 `configure_obs()`：
     CMake 系依赖的编译器由 NDK toolchain 指定为绝对路径，`$CC` 对它们无效，所以显式
     挂 `CMAKE_{C,CXX}_COMPILER_LAUNCHER=ccache`。
   - `ci-build-android.sh` 的 `setup_ccache()` 统一配置 `CCACHE_DIR` /
     `COMPILERCHECK=content` / `BASEDIR` / `NOHASHDIR` / `MAXSIZE=3G`（实测占用极小）。
   - workflow 用 `actions/cache` 存 `~/.ccache`，key 带 `github.sha` + `run_attempt`。
   - 所有注入都受 `USE_CCACHE` 开关控制，本机没装 ccache 时行为与原来一致。

## 本分支**没有**搬什么，以及为什么

- 前端 60 个上游文件的 `#ifdef` 清理、版权头统一、CODESTYLE（大括号/常量命名）、
  10 个 `OBSAndroid*` 目录分层、4 个插件属性页 locale 英文化、`obs.h` 平台 include 块
  —— 这些是 OBS-For-Android 对自己仓库的净化；本仓库 `obs-studio/` 子树已被
  MediaCodec / WebView / GPU 采集大改，且两条线基线不同，逐条适配的冲突面远大于收益。
- `.gitignore` 忽略 deps 产物、release keystore 参数化 —— 本仓库**已经有了**，不重复搬。
- 整仓覆盖（把 OBS-For-Android 的 HEAD 树铺过来）—— 会丢掉本仓库的硬编解码、
  浏览器源、GPU 采集全部功能，不可取。

## 其它可借鉴的工程化做法（未在本分支改动）

- **快速失败闸门**：在装 Qt 之前先用 `-DENABLE_FRONTEND=OFF` 编一遍 libobs / GLES /
  插件，几分钟内暴露原生层编译错误，同时验证"原生层不依赖 Qt"。本仓库的
  `ci-build-android.sh` 已有宿主工具自检（`probe_host_tools`），是同一套思路。
- **去掉整包 deps 缓存**：OBS-For-Android 选择每轮完整重编依赖，让 x264/rnnoise 跟随
  `master` 的上游更新自动生效，代价靠 deps 层 ccache 抵消（冷启动 ~4 分钟 → 命中后
  ~1.5 分钟）。本仓库目前仍缓存 `deps-android/prebuilt`，是否采用需权衡。
- **`permissions` 收窄 / `paths` 白名单 / actionlint 静态校验**：本仓库 permissions 已做，
  paths 与 actionlint 可按需补。

## 建议的后续

1. 先在 Issue 里确认协作意向与分工（基线与布局差异决定了不能整仓合并）。
2. 本分支可作为第一个 PR 的候选，已按「fdk 合规」「归档校验 + 构建健壮性」
   「ccache」拆成三个独立提交，便于 review 与单独 revert。
3. 若要把本仓库的功能（MediaCodec / WebView / GPU 采集）反向移植到 33.0.1 基线，
   建议按"设计 + 补丁"交换，而不是 cherry-pick。
