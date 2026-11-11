# Engineering Collaboration Proposal / 跨分支工程化适配提案

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This proposal outlines the design and plan for adopting the build system, compliance, and caching improvements from [TTTornado/OBS-For-Android](https://github.com/TTTornado/OBS-For-Android) into this repository.

### Background and Architectural Differences

Both forks originate from `cunminF/OBS_for_Android`, but have evolved along different architectural paths without a common Git history:

| Dimension | This Repository | TTTornado Fork |
| --- | --- | --- |
| **Baseline** | OBS 33.x, preserving commit history | OBS 33.0.1, independent fork |
| **Layout** | Subtree layout (`deps-android/` + `obs-studio/`) | Root-level layout |
| **Feature Focus** | MediaCodec hardware codecs, WebView, GPU capture, UVC pipeline | Build-script refactoring, code hygiene, and CI optimization |

Because direct `git cherry-pick` is impractical across layout boundaries, modifications are ported as modular, targeted changes.

### Recommended Improvements

1. **License & Distribution Compliance**:
   * Removed external audio dependencies with conflicting terms, ensuring clean compliance with GPLv2+ for distributed binaries.
2. **Download Archive Integrity Verification (`fetch.sh`)**:
   * Introduced full archive listing (`tar -t`) after dependency tarballs are downloaded. This prevents network timeouts, truncated archives, or proxy HTML error pages from contaminating the build cache.
3. **Build Script Robustness (`build.sh`, `defs.sh`)**:
   * Resolved subshell exit-code masking in conditional blocks, ensuring all native compilation steps and header copies propagate failures immediately (Fail-Fast).
4. **Three-Tier ccache Compilation Caching**:
   * Wraps compiler calls for Autotools/Makefile dependencies;
   * Injects `CMAKE_{C,CXX}_COMPILER_LAUNCHER=ccache` for CMake dependencies;
   * Integrates GitHub Actions cache for CI runners, significantly reducing build times.

### Recommendations for Upstream Collaboration

* **Preserve Core Feature Pipelines**: The MediaCodec, GPU capture, and UVC acceleration changes in this repository deeply customize libobs. Wholesale tree replacement must be avoided.
* **Modular PR Staging**: Recommend submitting compliance fixes, download verification, and ccache integration as independent pull requests for easier review and maintenance.

---

<a name="简体中文"></a>
## 简体中文

本提案阐述将 [TTTornado/OBS-For-Android](https://github.com/TTTornado/OBS-For-Android) 分支中的构建体系、合规性与编译缓存改进，适配移植至本仓库架构的设计方案。

### 背景与架构差异

两条开发分支均源自 `cunminF/OBS_for_Android`，但在代码组织与基线上存在结构性差异：

| 维度 | 本仓库 | TTTornado 分支 |
| --- | --- | --- |
| **基线版本** | OBS 33.x，保留完整历史 | OBS 33.0.1，从 upstream 独立分化 |
| **目录架构** | 子目录布局（`deps-android/` + `obs-studio/`） | 根级展开布局 |
| **功能特性** | 重点实现 MediaCodec 硬编解码、WebView、GPU 采集与 UVC 优化 | 重点进行构建脚本重构、代码规范化与 CI 优化 |

鉴于两分支无法直接通过 `git cherry-pick` 无损合并，相关改动需采取模块化、针对性的工程化提取。

### 推荐采纳的工程化改进

1. **移除不兼容依赖（合规性）**：
   * 移除了分发许可与 GPL 存在潜在冲突的外部音频依赖组件，确保最终生成的 APK 具备完整的开源分发合规性。
2. **依赖下载完整性校验 (`fetch.sh`)**：
   * 在依赖源码包下载完成后引入全量归档解压探测（`tar -t`），有效拦截由于网络抖动导致的截断包或代理异常返回的 HTML 页面，避免损坏文件进入构建缓存。
3. **构建脚本健壮性提升 (`build.sh`, `defs.sh`)**：
   * 消除子 shell 条件执行时的错误掩盖问题，明确检查各构建步骤与 Header-only 依赖拷贝的退出码，确保原生编译错误立即触发快速失败（Fail-Fast）。
4. **三层 ccache 编译缓存集成**：
   * **Autotools / Makefile 系**：通过包装编译器 Wrapper 接入 ccache；
   * **CMake 系**：显式注入 `CMAKE_{C,CXX}_COMPILER_LAUNCHER=ccache`；
   * **CI 工作流**：通过 GitHub Actions 缓存机制保留编译缓存，将重复构建时间大幅缩减。

### 协作与推进建议

* **避免整仓覆盖**：本仓库的 MediaCodec 硬编解码、GPU 屏幕采集直连及 UVC 高性能通道修改深入 libobs 核心，整仓替换会导致关键功能丢失。
* **分模块 PR 推进**：建议将合规清理、下载校验与 ccache 加速拆分为独立的 PR 分批合入，以降低评审与维护成本。
