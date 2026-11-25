# 更新日志

以下记录整理自本仓库主要发布节点与工程提交历史（完整提交记录见 [GitHub Commits](https://github.com/Keee7/OBS-for-Android/commits/main) 与 [Releases](https://github.com/Keee7/OBS-for-Android/releases)）。

## v0.2.8（当前基线）

* **UVC 双后端架构与大载荷 Bulk 相机支持**：
  * 内置原生 UVC 引擎新增标准 Bulk Payload 重组器与 ZLP 边界透传，原生支持 Sony A7M4 等 `dwMaxPayloadTransferSize > 64 KiB` 的大载荷 Bulk 相机，修复重插拔后属性页【申请 USB 授权】沿用旧路径的问题。
  * 内嵌 Android 加固版 `libuvc 0.0.9`（64 KiB 分块 Bulk、8 包 ISO 上限、真实端点类型判定、有界超时停止与后台延迟回收），在 UVC 源属性页提供「内置原生引擎（默认）/ libuvc 兼容引擎」即时切换。
  * 双后端统一新增 `UYVY` 未压缩格式支持，补齐 `P010` 硬件纹理能力过滤与 `libuvc` 路径黑屏自动切换 `YUYV`。
* **原生 NDI® 局域网收发（BYOR）**：新增 `android-ndi` 原生插件，支持 NDI® 输入源接收（局域网 `NsdManager` 自动发现与手动 IP 发现）及主程序 NDI® 输出（独立发送线程 + 有界队列）；内置官方 `libndi.so` 导入向导，自动校验 ARM64 ABI、16 KB 页对齐与必需符号。
* **常亮防息屏与 UVC YUV 体验优化**：新增屏幕常亮（防息屏）开关，避免长时间直播/录屏自动熄屏；优化 UVC YUV 档位锁定与最低延迟提示说明。
* **双语使用文档站点**：上线中英双语 VitePress 使用教程站点与 GitHub Pages 自动部署。

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
