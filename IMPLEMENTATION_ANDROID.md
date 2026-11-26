# Android Port Architecture and Implementation / 平台移植与技术实现规范

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This document provides a technical overview of the architectural changes, hardware acceleration pipelines, and platform adaptations implemented in OBS for Android.

### 1. Hardware Video Acceleration Pipelines

#### 1.1 MediaCodec Hardware Encoding (`android-mediacodec`)
* **Direct Surface / GPU Texture Input**: libobs is extended with direct GPU texture encoding capabilities. Composited frames from the rendering pipeline are submitted directly to the MediaCodec input EGL Surface, eliminating CPU memory round-trips.
* **Shared EGL Context & Synchronization**: The encoder Surface uses an independent shared EGLContext with dual-direction GPU sync fences. Blocking `eglSwapBuffers` calls do not lock or stall the primary OBS rendering loop.
* **Rate Control & Quality Mapping**: Supports CBR, VBR, and CQ (Constant Quality) modes. Abstract quality presets (Automatic, Low, Medium, High) are dynamically mapped to the device component's complexity and quality ranges.
* **Session Lifetime & Robustness**: Implements generational session identifiers and protected state transitions to prevent obsolete asynchronous callbacks from interfering with newly initialized encoder instances.

#### 1.2 GPU Screen Capture (`android-screen`)
* **MediaProjection Pipeline**: Video frames captured via MediaProjection are bound to a `SurfaceTexture` / OES texture and fed directly into the OBS compositing pipeline.
* **Session Lifecycle**: Complies with Android 14+ specifications by maintaining a single persistent `VirtualDisplay` instance per authorization. Screen rotations and resolution changes resize and rebind surfaces dynamically without re-prompting the user.
* **Alpha Channel & Transparent Compositing**: Integrates with Android 14+ `onCapturedContentResize` callbacks. Provides Straight Alpha and Premultiplied Alpha compositing modes for transparent window capture (e.g., avatar capture tools and transparent UI overlays).

#### 1.3 WebView Browser Source (`android-browser`)
* **Rendering Path**: Hosts the Android System WebView in a dedicated virtual display and converts output to an OBS-compatible GPU texture.
* **Zero-Copy Transparent Background**: In transparent mode, utilizes `ImageReader (RGBA_8888)` + `AHardwareBuffer` sampled via EGLImage. SurfaceFlinger compositing retains alpha transparency with zero CPU memory copying.

---

### 2. Media Streaming, Storage, and Ingestion

#### 2.1 Scoped Storage & Direct Recording
* **SAF & MediaStore Bridge**: Maps public paths (such as `Download/obs`) to seekable Android File Descriptors (FD).
* **Native Direct I/O**: The OBS MP4 buffered writer streams directly into the granted file descriptor, finalizing MP4 container packaging in-place without intermediate staging copies.
* **Capacity Probing**: Replaces POSIX `statvfs` with Android's `StatFs` service, enforcing a 50 MiB low-space safety floor while preventing false-positive "insufficient disk space" stops.

#### 2.2 High-Speed UVC Ingestion
* **Decoupled Worker Threads**: Completely separates `libusb` event handling from frame parsing and assembly. Employs pre-allocated ring buffer pools with zero-copy ownership handoffs.
* **Raw YUV Direct Assembly**: Extracts raw YUV (YUYV / NV12) directly from standard bulk payload packets and submits frames straight into OBS GPU texture conversion.
* **Stall Watchdog & Automatic Recovery**: Monitors valid video frame cadence. If frame arrival stalls or USB endpoints halt, the pipeline automatically clears halts and renegotiates connections cleanly.

---

### 3. Responsive Layout & Mobile UI Adaptations

#### 3.1 Responsive Landscape Engine (`OBSAndroidLayout`)
* **Breakpoint & Hysteresis**: Evaluates window geometry using Qt logical pixels (`952×552` breakpoint with a 48px hysteresis band). Smoothly transitions between a **compact landscape mode** (for smartphones, split-screen, and small floating windows) and a **spacious layout** (for tablets and large displays).
* **Space Optimization**: Compact mode adjusts list row heights from 48px to 32px and automatically collapses idle toolbars to maximize video canvas preview area.
* **Clamped Dialog Geometry (`fitLimit`)**: Constrains properties dialogs to the intersection of the visible window and screen geometry, preventing controls from extending off-screen.

#### 3.2 Qt Window Coordinates & IME Handling
* **Unified Window Geometry**: Standardizes parent and preview coordinates using `getLocationInWindow`, eliminating alignment offsets across fullscreen, split-screen, and floating window modes.
* **IME Event De-duplication**: Filters redundant `showSoftInput` request storms triggered by certain soft keyboard backspace events while keeping text, cursor, and selection state fully synchronized.

---

### 4. Bundled Plugin Loading Chain

The order below is the real startup order; every step maps to code in this tree.

1. `scripts/android-plugin-bundle.py` — `generate` walks `assets/obs-plugins/*.so`, enforces the required set (`android-browser`, `android-mediacodec`, `android-capture`, `obs-ffmpeg`, `obs-outputs`, `obs-x264`), rejects anything that is not an AArch64 shared object, and writes one line per plugin. `verify` recomputes the same lines from a built APK and fails the build on mismatch.
2. `MANIFEST` — `assets/obs-plugins/MANIFEST`, one `<name>.so <sha256> <size>` line per bundled plugin. It is both the integrity contract and, through its own SHA-256, the on-disk name of the published bundle directory.
3. `AndroidRuntimeBootstrap` — `EnsureAndroidPluginBinaries()` runs in `OBSApp::loadAppModules()` before any module is loaded. It parses the MANIFEST (malformed, duplicate or empty entries are rejected). When a bundle must be extracted, each `.so` is streamed out with its SHA-256 and byte count checked against the MANIFEST, staged in `.staging-XXXXXX`, and published with a single `rename()` only after every file verified. **The reuse path has a narrower trust boundary:** an already published `bundled-plugins/<MANIFEST SHA-256>/` directory is accepted after checking only the `.complete` marker, that each entry is a regular non-symlink file, and that its size matches the MANIFEST. It is not re-hashed, so equal-length corruption or tampering in an existing directory can pass the reuse check; hashes are enforced only when extracting a new bundle.
4. `OBS_ANDROID_BUNDLED_PLUGIN_DIR` — the published `$OBS_ROOT_PATH/bundled-plugins/<MANIFEST SHA-256>` directory, exported after extraction under `.extract-lock`.
5. `PluginManager::loadAllPlugins` — `preLoad()` reads `modules.json` and applies the disabled list, then `obs_load_core_modules()` loads the core set from the bundle. The optional-plugin scan (`loadPlugins()`) runs only in the plugin manager's Full mode: `OBSApp::loadAppModules()` selects CoreOnly when safe mode or "disable third-party plugins" is active, and CoreOnly skips optional plugins entirely. In Full mode the bundle is scanned before `$OBS_ROOT_PATH/lib`, so a bundled plugin wins over a user-installed one with the same module name.
6. `obs_load_plugins` — libobs enumerates the directory (`find_modules_in_path` → `load_all_callback`), skips names that are already loaded, honours the disabled list, and records the failures that happen before init (open failure, incompatible version, unknown error) in `obs_module_failure_info`; the frontend logs those names and reports `PartialFailure` when the count is non-zero. A module that opens but then fails `obs_init_module()` is not counted there: it is registered as a disabled module flagged `OBS_MODULE_FAILED_TO_INITIALIZE`, so it never enters the failed-module list and does not by itself drive `PartialFailure`.

Kept behaviour: MANIFEST SHA-256 verification (extraction path only), atomic directory publication, the core/optional split (the CMake `OBS_MODULES_ENABLED` list becomes `obs_core_modules[]` and is loaded as core modules — never listed in the plugin manager, never user-disableable — while the remaining bundled modules are loaded as optional plugins that do appear there and honour the disabled state), the disabled state stored in `modules.json`, bundled-over-user precedence, and failure reporting that distinguishes counted pre-init failures from init failures flagged on disabled modules.

---

<a name="简体中文"></a>
## 简体中文

本文档系统阐述 OBS for Android 的核心架构改造、硬件加速管线以及 Android 平台关键特性的工程实现。

---

### 1. 硬件编解码与加速管线

#### 1.1 MediaCodec 硬件编码集成 (`android-mediacodec`)
* **Surface / GPU 纹理直连输入**：libobs 扩展了 GPU 纹理直连编码能力。渲染合成结果直接交由 EGL Surface 输入 MediaCodec 编码器，避免将完整图像逐帧回读至 CPU 内存再上传。
* **独立 EGL 共享上下文**：编码 Surface 采用独立的共享 EGLContext 和双向 GPU Fence 同步，耗时较长的 `eglSwapBuffers` 操作不阻塞主渲染合成线程。
* **码率与质量映射**：
  * 支持 CBR、VBR 和 CQ（恒定质量）模式；
  * 将界面中的“自动 / 低 / 中 / 高”质量档位动态映射到底层硬件报告的 complexity 与 quality 参数区间。
* **会话生命周期与鲁棒性**：
  * 采用代次识别（Session Generation）与状态保护锁，防止旧会话异步回调影响新创建的编码流程；
  * 编码停止时由最终活跃的输出通道统一下发 EOS，避免多路输出时过早关闭硬件编码器。

#### 1.2 GPU 屏幕采集 (`android-screen`)
* **架构设计**：基于 Android MediaProjection API，将采集缓冲区绑定到 `SurfaceTexture` / OES 纹理，直接挂载进 OBS 渲染管线。
* **生命周期与自愈**：
  * 严格遵循 Android 14+ 规范，单次系统授权仅对应单个 `VirtualDisplay` 实例；屏幕旋转或分辨率变更时，动态 resize / rebind Surface，不重复请求授权；
  * 采集故障采用指数退避重试，避免异常时频繁拉起系统授权弹窗。
* **Alpha 通道与透明合成**：
  * 接入 Android 14+ `onCapturedContentResize` 回调，单应用窗口变化时自动匹配有效内容尺寸；
  * 支持直通 Alpha（Straight）与预乘 Alpha（Premultiplied）合成模式，适用于虚拟主播动捕形象、透明 UI 浮层等叠加场景。

#### 1.3 WebView 浏览器源 (`android-browser`)
* **渲染路径**：在独立虚拟显示屏中托管 Android 系统 WebView，通过 GPU 纹理回传给 OBS。
* **零拷贝透明支持**：
  * 透明模式下通过 `ImageReader (RGBA_8888)` + `AHardwareBuffer` 经 EGLImage 直接采样，利用底层 SurfaceFlinger GPU 合成保留 Alpha 通道，实现零 CPU 内存拷贝；
  * 支持自定义 CSS 注入、帧率上限限制与基础触摸事件转发。

---

### 2. 媒体流传输与存储适配

#### 2.1 存储与分区存储 (Scoped Storage)
* **SAF / MediaStore 桥接**：新增存储桥接模块，将公共文档路径（如 `Download/obs`）映射为系统可 seek 的文件描述符 (File Descriptor)。
* **原生写入优化**：OBS MP4 缓冲写入器通过一次 JNI 传递直接操作底层文件描述符，录制结束原地完成封装，无需在临时私有目录下二度转存。
* **容量检测容错**：通过 `StatFs` 直接查询实际存储卷剩余空间，设立 50 MiB 安全保护阈值，修复标准 POSIX `statvfs` 在 Android SAF URI 下返回容量为 0 导致录像误判中断的问题。

#### 2.2 UVC 采集卡高速数据通道
* **双线程解耦设计**：`libusb` 事件接收线程与数据包解析线程完全解耦，预分配内存缓冲池，避免在传输回调中执行耗时拷贝。
* **Raw YUV (YUYV / NV12) 快速通路**：
  * 依据 UVC 标准载荷结构直接重组 YUV 帧，并保持 NV12/YUYV 格式直入 OBS GPU 纹理转换队列；
  * 智能断流看门狗：监控有效视频帧率，当持续多秒无有效帧到达或 USB 端点卡死时，自动排空 USB 管道并执行限频软重置恢复。

---

### 3. Qt 界面与移动端响应式布局

#### 3.1 手机与大屏横屏响应式布局 (`OBSAndroidLayout`)
* **逻辑像素断点与滞回保护**：
  * 基于 Qt 逻辑像素（而非绑定特定物理型号），当可用宽度 `< 952` 或高度 `< 552` 时自动进入**紧凑模式**；当窗口扩大至 `≥ 1000 × 600` 时恢复**宽松模式**；
  * 48px 滞回区间彻底消除窗口微调与分屏缩放时的界面抖动。
* **手机横屏空间利用率优化**：
  * 紧凑模式下触控行高压缩为 32px，并自动折叠未选中状态的上下文工具栏，最大化视频画布预览面积。
* **弹窗边界约束 (`fitLimit`)**：
  * 属性与设置对话框严格限制在窗口可视可用区内，避免在小屏或分屏模式下出现弹窗溢出。

#### 3.2 坐标几何与输入法适配
* **统一坐标系统**：统一采用 `getLocationInWindow` 作为基准窗口坐标系计算，将 Qt 界面位置与 Android 原生父容器精准对齐，彻底解决悬浮小窗与分屏拖拽时的画面错位问题。
* **软键盘优化**：针对部分输入法软键盘退格时多余触发的显示请求进行针对性防护，并完善光标手柄的焦点清理机制。

---

### 4. 随包插件装载链

下面的顺序就是实际启动顺序，每一步都能在代码树里核对：

1. `scripts/android-plugin-bundle.py` —— `generate` 扫描 `assets/obs-plugins/*.so`，校验必需插件集合（`android-browser`、`android-mediacodec`、`android-capture`、`obs-ffmpeg`、`obs-outputs`、`obs-x264`），拒绝任何不是 AArch64 共享对象的文件，逐个插件写一行清单；`verify` 在构建出的 APK 上重算同样的行，不一致就让构建失败。
2. `MANIFEST` —— `assets/obs-plugins/MANIFEST`，每个随包插件一行 `<模块>.so <sha256> <字节数>`；它既是完整性契约，又通过自身 SHA-256 决定发布目录在磁盘上的名字。
3. `AndroidRuntimeBootstrap` —— `EnsureAndroidPluginBinaries()` 在 `OBSApp::loadAppModules()` 里、任何模块装载之前执行：解析 MANIFEST（格式错误、重复项、空清单一律拒绝）。**只有需要解包时**才逐文件流式校验 SHA-256 与字节数，先解到 `.staging-XXXXXX`，全部校验通过后才用一次 `rename()` 原子发布。**复用路径的信任边界更窄**：已发布的 `bundled-plugins/<MANIFEST 的 SHA-256>/` 目录只检查 `.complete` 标记、每项是否为非符号链接的普通文件、以及大小是否与 MANIFEST 一致，**不会重新计算哈希** —— 因此已有目录里等长的损坏或篡改能通过复用检查；哈希只在解包新 bundle 时强制。保留 MANIFEST 校验与原子目录发布：解包路径上残缺的 bundle 不会被发布，复用路径则以上述边界为准。
4. `OBS_ANDROID_BUNDLED_PLUGIN_DIR` —— 发布目录 `$OBS_ROOT_PATH/bundled-plugins/<MANIFEST 的 SHA-256>`，在 `.extract-lock` 保护下解包成功后导出到环境变量。
5. `PluginManager::loadAllPlugins` —— `preLoad()` 读取 `modules.json` 并落地禁用列表，`obs_load_core_modules()` 从 bundle 装载核心模块。可选插件扫描（`loadPlugins()`）只在插件管理器的 Full 模式下运行：`OBSApp::loadAppModules()` 在安全模式或「禁用第三方插件」时选择 CoreOnly，而 CoreOnly 完全跳过可选插件。Full 模式下随包目录先于 `$OBS_ROOT_PATH/lib` 扫描，因此同名模块随包优先于用户自装。
6. `obs_load_plugins` —— libobs 侧 `find_modules_in_path` → `load_all_callback` 遍历目录，跳过已装载的同名模块、遵守禁用列表，并把**初始化之前**的失败（打开失败、版本不兼容、未知错误）记进 `obs_module_failure_info`；前端逐条打日志，计数非零时返回 `PartialFailure`。能打开但 `obs_init_module()` 失败的模块**不计入**该结构：它会被登记为标记 `OBS_MODULE_FAILED_TO_INITIALIZE` 的禁用模块，因此不会进入失败模块清单，也不会单独触发 `PartialFailure`。

保留项：MANIFEST SHA-256 校验（只在解包路径执行）与原子目录发布、核心/可选分类（CMake 的 `OBS_MODULES_ENABLED` 会生成 `obs_core_modules[]`，按核心模块装载 —— 不进插件管理器、不受用户禁用配置影响；其余随包模块按可选插件装载，会出现在插件管理器里并遵守禁用状态）、`modules.json` 里的禁用状态、随包优先于用户插件、失败记录（`obs_module_failure_info` 只收初始化之前的失败，初始化失败改为禁用模块上的标记）。
