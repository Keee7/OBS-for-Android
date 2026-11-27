# 更新日志

以下记录整理自本仓库主要发布节点与工程提交历史（完整提交记录见 [GitHub Commits](https://github.com/Keee7/OBS-for-Android/commits/main) 与 [Releases](https://github.com/Keee7/OBS-for-Android/releases)）。

## v0.2.10（当前基线）

* **MediaCodec 硬件编码兼容与诊断**：为能力声明存在偏差的硬件编码器新增实际 `configure()` 探测回退，补齐编码器能力日志，并区分码控、质量、分辨率/帧率、码率等具体报错原因。
* **MediaCodec 硬解与渲染修复**：修复 Android EGLImage 导入、fence 同步与纹理/FBO 销毁残留；为媒体源新增 MediaCodec Surface 零拷贝开关（支持切换非 Surface 硬解）与分阶段诊断日志。
* **NDI®|HX 硬解与运行库加固**：新增 NDI®|HX MediaCodec 硬件解码，修复额外 IP 的输入法问题，并将 NDI 运行库加载限制在应用私有目录。
* **浏览器、录屏与默认推流**：浏览器交互新增返回/刷新按钮与触控手势；修复横屏录屏尺寸与授权异常处理；默认推流服务改为“自定义”。
* **工作室模式上下布局**：在工作室模式转场栏 `⋮` 菜单新增「开启纵向布局」切换（与通用设置同步），平板/大屏横屏时可切换为类似手机的上下堆叠布局以增大 16:9 画面占比，并修复切回左右布局时的 Qt 焦点崩溃。
* **架构与构建维护**：模块化拆分 Android 前端与 Java 宿主类，统一 JNI 上下文与清单版本号来源，CI 工具链升级至 JDK 21。

## v0.2.9

* **UVC 低延迟平滑缓冲**：新增按时间预算控制的异步缓冲与自动追帧，并升级批量帧时间戳平滑，显著提升采集流畅度并加快卡顿恢复。
* **UVC 级联控制与 USB 安全弹出**：将 UVC 参数拆分为格式 → 分辨率 → 帧率级联选择，统一 `libuvc` 与原生后端锁帧行为，自动选设备时过滤非视频 USB 外设；为 UVC 与尼康源新增安全弹出 / 重连功能，并修复重复弹 USB 授权的问题。
* **浏览器桌面模式**：浏览器源新增桌面模式（支持 Client Hints 与桌面视口）及适应宽度选项。
* **NDI® 与常规维护**：NDI® 设置改为直连官方 NDI v6 Android SDK 压缩包，修复重复发现 NDI 源，加固 USB / 存储宿主桥并完善插件装载文档与 SPDX 声明。

## v0.2.8

* **UVC 双后端与大载荷 Bulk 兼容**：新增“内置原生引擎（默认）/ `libuvc` 兼容引擎”即时切换；内置引擎支持大载荷 Bulk 重组与 ZLP 帧边界，补齐 `UYVY` 格式、`P010` 能力过滤与重连后 USB 授权路径刷新，并加固 `libuvc` 停流回收与黑屏回退。
* **原生 NDI® 局域网收发（BYOR）**：新增 NDI® 输入源（自动发现与手动 IP）与主画面 NDI® 输出，支持应用内安全导入官方 `libndi.so`。
* **屏幕常亮与 UVC 选项优化**：新增运行期间保持屏幕常亮开关，优化 UVC YUV 档位锁定与最低延迟提示说明。

## v0.2.7

* **UVC 采集增强**：新增 UVC Isochronous（等时）传输采集支持，并强化 Bulk 传输与 USB 断开清理时的稳定性。
* **SRT 媒体播放修复**：优化媒体源拉取 SRT 流时的 MediaCodec Surface 硬解与 GPU 同步稳定性。
* **编码与色彩范围完善**：支持按色彩空间与码率模式探测 `Limited / Full` 色彩范围，硬件不支持 Full Range 或 HDR Main10 时自动回退至 ByteBuffer YUV 或软件编码器，并增加界面与状态栏提示。
* **UI 渲染性能优化**：引入 Android RHI BackingStore 局部脏区合并上传，优化 `VolumeMeter`、`MediaControls` 与 `AbsoluteSlider` 局部重绘，显著降低空闲与高亮混音场景下的 CPU 占用。
* **关于与视觉更新**：改进关于对话框信息展示与 `AUTHORS` 内置打包，更新 README 双语排版与 WebP 界面预览图。

## v0.2.6

* **应用内更新检查**：新增 GitHub Releases 启动自动检查与手动「检查更新」弹窗入口。
* **UVC 音频自动匹配加固**：按 USB 路径与设备 `VID:PID` 严格校验匹配自带 UAC 声卡，将音频重试移出视频线程防止卡顿。
* **媒体源增强**：支持 MediaCodec Surface 零拷贝解码并优化 P010 HDR 路径、异步滤镜与结束留帧逻辑；日志中自动对推流 URL 与 FFmpeg 选项脱敏。
* **RTMP 清理修复**：修复 RTMP 断开连接时的清理与缓冲控制包处理。

## v0.2.5

* **Qt 6.12.0 LTS 升级**：全面升级至 Qt 6.12.0，提升移动端设置页加载速度与下拉列表滚动稳定性。
* **MediaProjection / HDR / SAF 集成**：完善 Android 14+ 单应用窗口尺寸自适应与 Alpha 透明合成、10-bit HDR（Rec. 2100 PQ/HLG + P010/I010）、SAF 文档选择器导入/导出配置文件与场景集合。
* **SRT 媒体源支持**：在 Android 依赖链引入 MbedTLS、SRT、librist 与 libxml2，支持网络流硬解播放。
* **手机紧凑布局与工作室模式**：优化小屏「工作室模式」切换稳定性，并在紧凑工具栏中保留媒体播控按钮。
