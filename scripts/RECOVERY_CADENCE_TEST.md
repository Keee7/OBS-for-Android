# 0.1.22-cadence-diagnostics / 23

以下记录中的版本 23 APK 使用 `com.antutu.ABenchMark`；这是该历史构建的实际应用 ID。当前源码默认恢复为 `com.obsproject.studio`，Windows 可加 `-AntutuPackage` 生成 Antutu 包；Java/JNI 类命名空间始终保持 `com.obsproject.studio`。版本 23 的设备运动样本来自此前版本 21/22，不能作为新包名版本的实机验收。新包首次安装被系统拒绝，随后用户明确选择暂不安装测试，未再重试。

## 到帧抖动与重复帧

0.1.20 用于初轮修复验证；0.1.21 额外将 UVC 的 raw 格式协商/解析与“只取最新帧”分开。默认使用 OBS 的时序缓冲，保持所选 YUYV/NV12、尺寸、帧率；“最低延迟（优先最新帧）”仍可显式开启，但会丢弃排队中间帧，可能增加重复与卡顿。缓冲有延迟与内存代价，不保证任意负载下均输出独特 60 fps。

`scripts/test-uvc-cadence.py` 直接提取生产 `ready_async_frame` / `get_closest_frame`：10000 次 60 fps 输入/输出、相邻到帧时间交替抖动 4 ms，latest-only 只显示 5000 个不同 ID，标准缓冲显示 9998 个。该确定性测试证明机制，不单独证明用户原视频的重复率。

## 修复范围

- EGL 预览交换失败后，持有的 swapchain 每秒最多重建一次窗口面，不再依赖新的 Java Surface 事件；真正销毁 swapchain 后终止重试。
- 编码时间记录只删除输出 PTS 的精确匹配项，保留尚待输出的 B 帧记录。诊断历史最多 4096 项，按批次淘汰，避免编码器丢帧后无限增长。
- CPU 与 GPU 编码入口共用同一有界时间记录函数，覆盖 MediaCodec 的 GPU 编码路径。
- MediaCodec 拒绝动态码率参数时保留最后接受的码率、警告并停用本会话后续动态更新；原输出停滞看门狗继续工作。它不保证组件已经执行了被拒绝的网络降码率要求。
- 标准 raw UVC 帧达到协商的完整像素长度即可交付，支持不发 EOF、仅 FID 切帧的设备；短帧、错误、溢出和接收缺口仍隔离。交付后发现同身份额外数据会计错，不能撤回已交付帧。非标准兼容字节流路径未强制改为 Raw。
- AAudio 回调计数采用原子访问；监护线程独占停滞计时状态，回调独占样本时钟状态，关闭后再重置。原 USB 异常超速回调防护保留。
- 获权且活跃的 Camera2/AAudio 输入源按需维持 camera/microphone 前台服务，包括仅预览采集。后台新增 while-in-use 类型延后到回到前台重试；类型表示活跃源的采集意图，不保证设备打开成功。检测周期约一秒。
- Android 11+ 本地外置 SAF 卷通过系统已挂载卷 UUID 匹配并查询容量；无法可靠查询的提供者仍表示未知，不误判为满盘。Android 10 非主卷 SAF 容量仍可能未知。
- package 操作也先运行增量编译，再从本次产物暂存插件。APK 验证排除 Gradle staging/strip 缓存，仅对照权威编译输出及重新 strip 后的结果；缺失输出即失败，并要求新增 MediaCodec 日志标记存在。
- 新增 UVC 输入内容采样、到帧间隔、原始 PTS/PROBE 频率及 OBS 选帧日志，用于定位实际重复帧；只作诊断，不按未验证的设备时钟重写时间戳。详见 [诊断方法](UVC_CADENCE_DIAGNOSTICS.md)。

## 本地签名

本地 Windows 构建默认读取环境变量 `OBS_ANDROID_SIGNING_CONFIG` 指定的仓库外配置；未指定时读取当前用户 Documents 下的 `release key.txt`。也可传 `-SigningConfigFile`。配置支持 JSON 或带标签的文本，字段为 `ANDROID_KEYSTORE_BASE64`、`ANDROID_KEY_ALIAS`、`ANDROID_KEYSTORE_PASSWORD`、`ANDROID_KEY_PASSWORD`。第一项也可指向含 base64 密钥库的文件。

密码通过进程环境交给 apksigner，不写入参数和构建日志；临时解码密钥库在签名后移除。密钥文件与配置不提交 Git。配置缺失会失败，不会静默换成另一把密钥。只有显式 `-DebugSigning` 才使用公开 debug 密钥。CI 继续使用独立的 GitHub Secrets 签名流程。

## 原断流日志的证据

用户提供的 2026-11-08 19-11-56 会话日志中：

- 20:07 的 HEVC 停止前，UVC NV12 仍为约 60 fps、186.7 MB/s、无 USB 错误，队列丢弃计数没有在该窗口增加；编码输入面提交均值约 39–41 ms。
- HEVC 报 pending input 超限，随后统计编码跳帧 2209/5890（37.5%），渲染延误约 0.1%。20:10 的 AVC 也出现提交均值约 41–43 ms 后 pending 超限。
- 这证明相应窗口存在编码提交与积压问题；这些计数不是采集图像逐像素重复率。静态桌面或低帧率 HDMI 内容也会产生相同帧，输入唯一帧率需用持续运动、带帧号的信号和录制样本另外验证。
- 原 0.1.19 安装包未包含降载逻辑；本版重新入包并严格检查 `dropped_load` 和动态码率错误恢复标记。降载优先保持连接，过载期间可能降低输出帧率，并不保证任意负载均达到 60 fps。

## 验证

`scripts/test-android-host.ps1` 覆盖读回调生命周期、UVC、屏幕尺寸/Alpha、降载与 EGL 重试。新增生产函数回归覆盖 B 帧重排、100000 次弃帧下诊断历史有界、动态码率接受与拒绝；签名配置解析使用虚构凭据测试。

### 设备证据（2026-11-09）

设备为 Xiaomi 25053RP5CC / Android 16 / 玄戒 O1；使用独立临时 profile，仅本地录制，不向真实直播平台发送内容。

| 检查 | 已确认结果 |
| --- | --- |
| HEVC 初轮（0.1.20） | 1920×1080 / 60 fps，文件 255.07 秒、15304 帧，完整解码退出 0、无错误；文件 PTS 单调、最大间隔约 16.667 ms。编码日志 submitted/output/delivered 均 15313、failed=0、dropped_load=0，编码滞后 4/15313。 |
| H.264 修复前运动样本（0.1.20，latest-only） | 文件 164.48 秒、9869 帧，完整解码无错误。前 10 秒的帧号 ROI 在 599 对相邻画面中有 133 对近重复（22.204%），最长连续近重复 2 对。 |
| 缓冲版本运动样本（0.1.21） | release 签名覆盖安装成功，日志确认 raw_packets=1、low_latency=1、unbuffered=0。有效运动样本 90.50 秒、5430 帧，完整解码无错误；前 10 秒同一 ROI 的 599 对画面中有 243 对近重复（40.568%），其中 138 对二值 ROI 相同。缓冲开关本身未解决实际重复帧问题，不能作为帧率修复验收。 |
| H.264 本轮编码链路 | 本地录制日志 submitted/output/delivered 均 5492、failed=0、dropped_load=0，编码滞后 5/5492、渲染滞后 5 帧（均约 0.1%）。前 10 秒 UVC 约 60 fps，累计 queue_drops/parser_gaps 未增长；这些计数无法单独区分采集内容重复与后续选帧重复。 |

ROI 初筛使用原始输出像素区域 880×180@(400,180)，不缩放，灰度阈值 150、相邻二值图差异不超过 316 像素（区域的约 0.2%）视为近重复；抽帧确认包含六位 FRAME ID。A/B 的二值 ROI 相同分别为 44/599（7.346%）和 138/599（23.038%）。这是该样本的局部指标，不是逐像素整帧冻结率或精确 OCR 帧号统计。

本轮正常负载测试没有触发降载，不代表已实机复现并消除长期过载；高负载降载/恢复有宿主故障注入测试。后台 Camera2、外置 SAF 卷、真实平台与长期温控仍需单独验证。

测试结束后已逐字节恢复原 user.ini 和两个场景文件，移除测试 profile 并重启原 profile。录制和分析保存在仓库外的本地调试目录。

### 当前安装包（versionCode 23）

`OBS-Android-arm64-hwcodec-browser-20261109-0722.apk`，97,693,726 字节，`0.1.22-cadence-diagnostics` / versionCode `23`，实际应用 ID 为 `com.antutu.ABenchMark`，使用用户指定的仓库外 release key。SHA-256：

```text
7be3b92d64594cca3f96016d8d618851c10fd385260376a8f003d24d33a67edf
```

完整 Windows native/Qt/Java/APK 构建、签名 v3、ZIP 16KB 对齐、Manifest 包名/版本、UVC 输入/PTS/选帧诊断标记、权威编译输出/fresh-strip 一致性核验通过。构建脚本后续默认禁用持久 Gradle daemon，避免 Qt 内部构建完成后输出句柄仍被占用；该脚本调整已通过 PowerShell 语法检查。

本版未完成设备安装或短录制；不宣称重复帧问题已解决。

### 已验证的旧包（versionCode 22）

`OBS-Android-arm64-hwcodec-browser-20261109-0227.apk`，97,693,726 字节，`0.1.21-cadence-test` / versionCode `22`，使用用户指定的仓库外 release key。SHA-256：

```text
e186671795ff406406fbc0d8ac6185f367f9d81dc6d025a4b9c531e9dff0effb
```

完整 native/Qt/Java 构建、签名 v3、ZIP 16KB 对齐及权威编译输出/fresh-strip 一致性核验通过。旧 0.1.19 APK 已作为负对照确认会被新校验拒绝。日志、录制、截图和用户配置备份只保存在忽略的本地调试目录，不加入 Git。
