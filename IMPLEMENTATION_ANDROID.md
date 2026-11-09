# Android 硬编与浏览器源：首版实现说明

基线：`307d9d2f5f49c1b91c2ebf3e6bf99b949ba3f917`。本地开发分支：`codex/android-hwcodec-browser`。目标为用户指定的小米 7S Pro / 玄戒 O1 / Android 16 / 1080p60。用户后续授权 USB 调试，已在连接的平板验证媒体硬解修复。

## 实现内容

- `android-mediacodec`：标准 MediaCodec H.264 硬编、Surface 输入、Java 硬件能力筛选、候选初始化失败后继续尝试其他硬件组件、CBR、动态码率、SPS/PPS、关键帧标记、无 B 帧时间戳、异步取包和 EOS 排空。明确限制为 SDR BT.709、8-bit 4:2:0；不支持的色彩空间和参数会报错。保留 x264 可选项。
- libobs 新增 RGB 纹理编码能力与独立视频 mix。最终合成图像经 GPU 纹理队列送往编码 Surface，不为编码下载整帧到 CPU。编码 Surface 使用独立共享 EGL 上下文和双向 GPU fence；可能阻塞的 `eglSwapBuffers` 不持有 OBS 合成锁。
- `android-browser`：系统 WebView 经独立 VirtualDisplay/SurfaceTexture 进入 OES 纹理，再转换为 OBS 纹理。实现 URL、尺寸、导入帧率上限、CSS、刷新、显隐、渲染错误重试、基础点击/拖动，以及 Android 交互窗口预览接线。提供透明、上下方向、Canvas、WebGL 和按钮测试页面。
- 前端接入：简单/高级模式的编码器选项、按真实参数重新检测硬件、新配置默认 1080p60（已有配置保留）；独立简单录制的 Small/HQ 分别为 8/12 Mbps CBR，同推流质量档复用推流编码器。
- 编码器停止时仅最后一个输出发出 EOS；GPU 编码失败转交异步处理，避免 GPU 线程自等/自 join。通过输出会话代次、停止通知锁与 checked-start/失败回滚，处理旧错误误停新会话以及多轨部分启动的问题。标准 MP4、FLV、RTMP、null 和测试 sink 已处理 capture 启动失败。

## 已执行的验证

最新源码 `0.1.22-cadence-diagnostics` / 23：按用户要求将应用 ID 改为 `com.antutu.ABenchMark`，Java/JNI 类命名空间保留。CPU/GPU 编码入口统一使用有界时间记录；新增 UVC 输入采样、到帧间隔、原始 PTS 和 OBS 选帧诊断，以实机证据定位重复帧。签名统一使用仓库外的用户 release 配置。

前版 `0.1.21-cadence-test` / 22：UVC 默认恢复时序缓冲，保留 raw 模式并独立提供最低延迟开关。生产 OBS 异步选帧函数的 60fps/4ms 抖动回归从 5000/10000 不同帧改善为 9998/10000，但实机运动样本仍有明显重复帧，不能据此宣称帧率问题修复。详细范围见 [本轮记录](scripts/RECOVERY_CADENCE_TEST.md)。

前版 `0.1.20-recovery-test` / 21：修复 EGL 主动限频恢复、B 帧时间记录与有界回收、动态码率拒绝策略、raw FID-only 支持、AAudio 共享计数竞争、按需后台采集类型和外置 SAF 卷容量查询。重新打包并以权威编译产物进行严格核验，修复旧插件被误验收的问题；默认使用仓库外的 release 签名配置。复现、测试范围及安装包见 [本轮记录](scripts/RECOVERY_CADENCE_TEST.md)。

前版 `0.1.19-alpha-stability` / 20：MediaCodec 新增高稳定降载闸门。实机日志显示 `eglSwapBuffers` 提交耗时从 1.7-2.2ms 恶化到 36-44ms 并持续一分钟，而 pending 输入始终停在 0-2 帧，最后直跳硬限导致`pending input exceeded its frame limit` 断流。现在按提交耗时与待编码深度主动丢帧保持连接，降载期间仍继续 `take_packet` 排空编码器输出并保留停滞看门狗，新增 `event=shedding|recovered` 转换日志与 `dropped_load` 计数；同时修正 `max_pending` 夹紧方向，并用 `da_erase_range` 修掉上游丢帧时`encoder_packet_times` 的无界增长。
前版 `0.1.18-alpha-vsync-fix` / 19：修复 Java USB 同设备借用互相关闭、断开后旧 fd 归还影响新连接的生命周期；统一无有效帧恢复，排干 USB 后重新协商，失败使用跨会话限频 reset 并完整关闭重枚举。协商保留真实错误、拔线与权限/参数错误快速退出。标准 raw payload 兼容缺少 EOH 和额外头填充；新增分类传输诊断、生产协商/Java借用和假时钟恢复测试。Windows完整构建、签名v3、ZIP16KB和原生库一致性验证通过。APK `releases/OBS-Android-arm64-hwcodec-browser-20261106-2313.apk`，SHA-256 `603f979edd39e55f13e55957af9f6eee882258ab8d58af947ad7790a1fccb4ba`。实际设备复测记录见 scripts/UVC_1080P60_TEST.md。

前版 `0.1.12-uvc60-test` / 13：针对 USB 5Gbps YUYV/NV12 1080p60 → MediaCodec H.264，新增独立 libusb bulk 接收泵和预分配有界缓冲所有权交接，过载序号缺口丢弃半帧；标准 raw payload 路径避免像素扫描和中间流缓冲拷贝，修正 PROBE 帧长/最大payload字段为18/22；旧设备解析改为指针消费、追加时才压实。默认开启的 YUV 低延迟选项只在明确选择 raw YUV 时生效，核对设备模式、禁止静默降分辨率/切MJPEG，OBS async优先最新帧；重协商前取消排干旧泵，新增独立采集帧率/USB错误/队列丢块/交付耗时指标。完整1080p YUYV/NV12伪头像素保真、收包生命周期/过载、生产模式/PROBE/平面与时间戳回归通过，完整APK构建、签名、ZIP16KB与原生入包一致性检查通过。APK `releases/OBS-Android-arm64-hwcodec-browser-20261105-2029.apk`，SHA-256 `72633b64511b31b97b5cb8f25b2651cca62db96ce86f46a2d8d00b354f9febf1`。未连接实际采集卡验证吞吐或延迟，参见 scripts/UVC_1080P60_TEST.md。


最新 `0.1.11-capture-alpha-test` / 12：屏幕源新增兼容、直 Alpha、预乘输入三种透明度处理。默认保持原绘制方式；GPU 使用默认 effect 与独立混合状态，CPU 仅显式预乘模式执行反预乘转换。公共 OES 着色器和浏览器源不变，GPU 不增加像素读回，仍默认开启、失败重试且不自动回退 CPU。接入 Android 14+ onCapturedContentResize，内容尺寸用于下一次采集面重接，避免单应用窗口仍按整屏比例采集；同一授权仍只创建一次 VirtualDisplay。主机生命周期/尺寸、CPU Alpha 与小窗坐标测试通过；ANGLE 等价 GLSL 覆盖 Alpha 0/128/255 在三种背景透明度下的 RGB/Alpha 合成。完整原生/Qt/Java/APK 构建、签名、ZIP16KB、包版本/代码入包与原生库一致性检查通过。最终包 `releases/OBS-Android-arm64-hwcodec-browser-20261102-2212.apk`，SHA-256 `25ce0a31a281170b54c76f604186a68b42f0166fb2cdb037c1ca593b4164dc61`。尚未验证 VTube Studio 的设备端透明帧，详见 scripts/TRANSPARENT_CAPTURE_TEST.md。

最新 0.1.10-window-stream-test / 11：USB 复现 0.1.9 小窗错位，Qt 窗口内目标与 Android 屏幕坐标混用。将父容器位置和初始测量校正统一到 getLocationInWindow，删除模式分支与增量缓存；截图验证全屏→小窗、移动、缩放和返回全屏均保持对齐。推流修复包括重连清空动态码率表、CQ 关闭无效动态码率，以及 headers/socket 读取失败早退路径释放出队包。故障注入回归验证各包恰好释放一次。完整构建/签名/ZIP16KB/包检查通过，最终包 releases/OBS-Android-arm64-hwcodec-browser-20261102-2022.apk，SHA-256 9b5a362f5323a58bb538a23a9e2b96277a75c3b9e92f3832dc1b67393d093c56。用户手动解锁后完成 USB localhost RTMP 实测：正常接收88.6秒、5317帧、60fps，完整解码无错；主动断线约1.267秒自动重连；限速2Mbps时实际编码输出5秒窗口最低1711kbps，解除后回升到约6Mbps。正常/重连/限速接收文件均首关键帧、DTS单调且最大间隔17ms。恢复原配置并逐字节核对，临时profile与USB转发删除。详细范围见 scripts/DEVICE_VALIDATION_WINDOW_STREAM.md；未测真实直播平台、Wi‑Fi丢包或长期温控。


最新 0.1.9-multiwindow-test / 10：用户确认 0.1.8 预览灰块已解决，但新增坐标同步导致小窗错位。按 Activity.isInMultiWindowMode 分支，小窗/分屏采用此前初始放置契约，不执行新父屏幕原点扣减；同一视图移动使用目标坐标增量，保留 attach 测量校正，空闲和改尺寸不覆盖位置。全屏保留 0.1.8 修正。新增小窗非零原点、空闲/尺寸变化、移动保留校正、全屏切换主机回归通过；实际 Java SDK 编译、完整构建、签名、ZIP16KB 与包内容检查通过。包 releases/OBS-Android-arm64-hwcodec-browser-20261102-1729.apk，SHA-256 02091cc2ce0d5bc9130a3ec3e449bfb422212ae1dfe3bf8213609bdb266e8a7f。未在设备验证本版小窗表现，厂商模式识别和实际几何仍需确认。

最新 0.1.8-preview-rect-test / 9：用户确认屏幕采集底部灰块仅影响预览。修正原生 SurfaceView 的 screen-to-parent margin 换算，保留状态栏/insets 校正，挂载与后续更新共用算法；排队任务读取最新几何，布局完成后更新真实测量。Qt 以每秒一次低频核对活动预览，覆盖祖先移动而子控件未收到 moveEvent 的情况。仅目标变化时调度尺寸自愈，避免轮询无限延期或重复错误日志。GPU 默认开启与失败重试不变。实际 Java 几何更新方法的主机回归、Android SDK Java 与完整原生/Qt/APK 构建、签名、16KB ZIP 对齐和包内容检查通过。包 releases/OBS-Android-arm64-hwcodec-browser-20261102-0726.apk，SHA-256 6254792bae7f9526d0967d32c21f42805a5993cee6d9fa84a34c26590d165a70。USB 无设备，无线配对地址不可达，用户要求不再连接、先编译；本版未实机验证灰块消失。

最新 0.1.7-xring-decode-test / 8：实机旧版首帧为 176×144，而系统玄戒解码组件随后输出 3840×2160。FFmpeg 输出格式解析未清空上一份可选 crop/stride/slice/display 字段，格式变化后残留初始尺寸。现已在每次解析前重置这些字段，重编实际 FFmpeg 依赖、OBS 与 APK。新增提取真实解析代码的格式变化回归通过。USB 覆盖安装后，同一 4K H.264 视频开启硬解显示完整画面，重启复测正常；两次真实日志均为 `c2.xring.avc.decoder hardware=true width=3840 height=2160 coded=3840x2160 linesize=3840,1920,1920`。未修改场景变换。APK `releases/OBS-Android-arm64-hwcodec-browser-20261102-0700.apk`，SHA-256 `2a317787b18730162c3203650445cd899a5432ba7583c9049a6066eb7b880b22`。签名、16KB 对齐与包内容验证通过；仅确认该设备/视频的播放几何，其他格式、编码性能与长期录制仍需各自回归。以下为各历史版本当时的验证记录。

最新 0.1.6-media-geometry-test / 7：FFmpeg MediaCodec YUV 裁剪拷贝修复（420 色度偏移、横向 crop 后整块复制问题）；OBS 媒体按实际帧几何、像素格式和色彩重建转换缓存；新增解码 buffer/stride/crop/visible 与首帧 coded/linesize/crop 诊断。主机旧 FFmpeg 负对照能检出裁剪错误，补丁通过；320×240 → 3840×2160 等转换状态回归通过。OBS 官方图标已加入应用及自适应图标资源。最终包 releases/OBS-Android-arm64-hwcodec-browser-20261102-0639.apk 完整构建、签名、16KB ZIP 对齐、原生/Java/图标入包检查通过；未在目标设备复现用户 4K 裁角问题，仍需实机复测。


最新交付 0.1.5-media-quality-test / 6：高级 MediaCodec 属性新增 CBR/VBR/CQ 与设备范围映射的质量档；GPU 屏幕源默认开启，失败退避重试，禁止自动 CPU 回退。媒体源 content URI 使用授权 FD/自定义 AVIO，FFmpeg 增加 codec_name 可复现补丁并已重编安装，NDK MediaCodec 按 SDK 报告为硬件的组件名创建，提供真实首帧日志；初始化硬解失败可明确回退软件。播放、定位、EOF、循环定位及重开/资源释放的宿主测试通过；完整 APK 构建、签名、ZIP/ELF16KB、依赖闭包与补丁入包校验通过。最新包 releases/OBS-Android-arm64-hwcodec-browser-20261102-0620.apk。设备侧媒体播放和质量控制仍待复测，详见 scripts/MEDIA_QUALITY_TEST.md。


最新为 `releases/OBS-Android-arm64-hwcodec-browser-20261102-0542.apk`，`0.1.4-hevc-test` / 5。MediaCodec 新增 HEVC Main/Main 兼容 8-bit SDR、VPS/SPS/PPS 检查、命名硬件组件的按码率/尺寸/帧率/CBR 能力筛选，简单/高级输出接线。普通编码访问单元只复制一次；首帧参数集提取不再把大关键帧复制进 extra。增加 5 秒无输出及有界输入积压保护、32 MiB 输出队列字节上限、暂停恢复等待窗口和专用 `OBS-MediaCodec` 日志。日志/录像验证工具现支持 HEVC；选择 MediaCodec 不会静默使用软件编码器。

用户要求的 GPU 屏幕采集开关已位于屏幕源属性，默认关闭，保留 CPU 兼容路径。开启后以 MediaProjection SurfaceTexture/OES 纹理接入合成，失败尝试回退；核心新增 `obs_source_draw_async_frame` 让自定义渲染路径可复用原有 CPU 异步帧绘制。新增 GPU Java/JNI/host bus，保持一份投影授权创建一次显示与按 Surface 身份解绑。修复音频泵持 sink 锁 join 的死锁；模式切换不在 graphics 锁内等待 reader 回调或音频 join。GPU 性能和设备颜色/方向/同步未实测。

本次主机回归覆盖 HEVC 参数集/真实 SPS 样本、混合首帧提取、无输出/慢输出积压/暂停、音频死锁负对照、GPU 首帧回退和 24 项验证器合成测试。完整 Android 原生/Qt/Java/Dex 构建、签名、ZIP/ELF 16KB 对齐、36 个库依赖闭包及新增 Java 类/原生接口验证通过。真实 HEVC 录制及 GPU 采集仍待玄戒实机验证，测试方法与优化空间见 `scripts/HEVC_HARDWARE_TEST.md`。

后续投影修复版为 `releases/OBS-Android-arm64-hwcodec-browser-20261102-0233.apk`，`0.1.3-projection-test` / 4。MediaProjection 的 VirtualDisplay 改为一份授权创建一次，场景恢复与尺寸变化只重接 Surface 并 resize；停用源时保留显示，结束授权时释放。修复不活动的源在 video_tick 自动重开，以及第二个源启动失败时关闭第一个源显示的问题。Java 异常通过新增错误接口显示在源属性中。默认屏幕采集在 Full HD 范围保持比例，可选原生分辨率；MediaCodec 简单设置隐藏无效的 x264 预设和参数，不再覆盖 x264 的预设配置。

投影会话和采集尺寸主机回归通过，完整 Android 原生/Qt/Gradle 构建、APK 签名、ZIP/ELF 16KB 对齐、36 个库依赖闭包、包内会话 Java 类和新接口验证通过，原生文件与最终构建匹配。最后新增的活动状态检查另做增量编译并重新打包核对。未连接 Android 实机，不能据此断言截图中的设备故障已完全消失；新版会展示具体系统异常以便进一步定位。详情见 `scripts/ANDROID_PROJECTION_TEST.md`。

2026-11-02 存储修复：新增 `ObsStorageHost` / Qt JNI 桥，将公共 Download、Movies、DCIM 路径映射到 MediaStore 文件，将授权的 SAF tree 映射到文档文件。原生 `os_fopen` 对 content URI 接入可 seek 文件描述符，现有 MP4 缓冲写入器直接输出，不增加完整录像复制。容量检查和统计窗口改用 Android 存储查询；未知容量不再当作零，真实低于 50 MiB 的保护仍保留。共享目录限制混合 MP4/MOV，不支持的分割、回放、FFmpeg 自定义输出明确提示；文档路径跳过自动 remux。完整回归步骤见 `scripts/ANDROID_STORAGE_TEST.md`。

本次已完成完整 CMake 原生/Qt 前端与 Gradle Java/Dex/APK 构建，APK v3 签名与 ZIP 16KB 对齐验证通过；36 个 AArch64 库的 ELF LOAD 对齐、动态依赖闭包、新增存储导出/引用和 `ObsStorageHost` Dex 类检查通过，包内原生文件与本次构建一致。新版为 `releases/OBS-Android-arm64-hwcodec-browser-20261102-0109.apk`，版本 `0.1.2-storage-test` / 3，校验值见安装说明。未连接实机验证共享目录写入和播放。

| 检查 | 结果和边界 |
| --- | --- |
| 官方 NDK r30 头、AArch64 Android API 29 对象编译 | 16 个 C 文件通过，包括修改的核心/图形链路、两个新插件和标准输出。使用 `-Wall -Wextra -Werror`；仅两个经核实存在于上游的告警分别保持可见、不升级为错误：`build_sprite_norm` 未使用和 `allow_clipping` 初始化器未指定 |
| Qt 6.9.3 Android arm64 真实 QtBase 头 + NDK | 两份新 JNI C++ 桥生成 AArch64 对象；检查使用 `-include arm_acle.h` 处理本机 MSYS Clang 与 Qt 头的 `__yield` 声明兼容性 |
| JDK 17 + Android 35 `android.jar` | 两份新增 Java 源码 `-Xlint:all` 编译通过，无警告 |
| 本机 ANGLE GLES 3 离屏 GPU 测试 | 直接解析实际 C 源中的 GLSL，三个 shader、两个 program 通过；线性→sRGB 色卡和 OES 预乘颜色读回符合预期。主机驱动为 Intel D3D11，不代表玄戒驱动 |
| AVC 主机测试 | Annex B、长度前缀、SPS/PPS、8-bit/10-bit/截断 SPS、分数帧率和时间戳溢出检查通过 |
| 录像验收工具测试 | 8 项通过：分数帧率、时长、帧数、时间戳顺序/间隙、冻结、重复帧与缺少散列 |
| Git diff | 空白错误检查通过；补丁另做基于原始索引的应用检查 |

此前已完成真实 CMake 配置、整套原生代码及 Qt 前端的编译链接，以及 Gradle APK 构建与开发签名。早期测试 APK 为 `releases/OBS-Android-arm64-hwcodec-browser-20261102-0038.apk`，现已由上面的存储修复版替代。安装说明和完整校验值见 `scripts/DEVICE_TEST_QUICKSTART.md`。原先的对象检查仅为早期检查，现已由完整构建进一步覆盖；仍未运行目标设备。

## 当前边界

- 未在目标设备运行，不能把能力声明或主机测试当成 1080p60 持续性能保证。颜色元数据、实际码率调整、方向、线程停止和后台渲染均需设备验证。
- 当前支持 H.264 和 HEVC Main/Main 兼容 8-bit SDR；未实现 HDR/10-bit、外部主动请求关键帧接口及厂商实际吞吐基准探测。
- 浏览器首版面向网页 URL。尚无本地 HTML 文件选择器；文件/内容访问禁用。明文 HTTP 受当前 Android 网络安全策略限制，测试页应通过可信 HTTPS 托管。
- 真正透明背景取决于 Android 虚拟显示是否保留 alpha。主机 OES 色彩测试通过不代表 WebView→虚拟显示在设备上已保留透明度。
- 没有独立网页 PCM 音轨，不提供键盘、滚轮或多点触控。网页内部 JS/GPU 工作量不受纹理导入上限完全约束。
- 屏幕源默认保留 CPU 通路，并有默认关闭的实验性 GPU 选项；相机源仍使用原 CPU 通路。未宣称整条采集链路零拷贝。
- 停止时发送 EOS 尾包不等于所有容器一定写入停止点之后的数据；现有 MP4 正常停止按时间截止，强制/异常停止有截断语义。第三方输出插件仍须正确处理 `obs_output_begin_data_capture` 返回值。
- 厂商系统调用若违反超时契约永久卡住，线程回收仍可能受影响。并发路径已做源码检查，尚未运行完整 OBS 并发压力测试。

## 复现检查与后续构建

检查工具仅位于仓库旁的 `../build-tools`，没有安装到系统或纳入源补丁。包含官方 NDK 头、SIMDe、uthash、Jansson 2.14、MbedTLS 3.6.4、QtBase Android arm64 6.9.3 头、JDK 和 Android 平台包。脚本参数允许更换本机 Clang/依赖根目录。

```powershell
./scripts/check-android-native.ps1
./scripts/check-android-jni.ps1
python scripts/test-android-interop-shaders.py --egl-dir 'C:\Program Files\Unity Hub'
python -m unittest discover -s scripts -p test_verify_1080p60.py -v
```

在具备完整 Qt/Android/WSL 及依赖库的环境按仓库 README 运行 `scripts/build-frontend-apk-wsl.sh`，从干净构建目录重建全部原生模块。此次改动包括核心接口及内部结构，不应只替换一个插件到旧 APK。安装与 60 分钟性能验收步骤见 `scripts/android-1080p60-acceptance.md`。

Windows 完整构建入口为 `scripts/build-frontend-apk-windows.ps1`；CMake 依赖与 MSYS FFmpeg/x264 构建脚本也已纳入 scripts。Windows 交叉构建的 FFmpeg/x264 查找器已按目标平台修正，USB 插件补链 Android 日志库。
