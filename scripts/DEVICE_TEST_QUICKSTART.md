# Android 测试包与实机记录

测试包：`releases/OBS-Android-arm64-hwcodec-browser-20261105-2029.apk`

- 应用版本：`0.1.12-uvc60-test`，versionCode `13`。
- 包名：`com.obsproject.studio`；arm64-v8a；最低 Android 10，目标 API 35。
- 文件大小：97,640,478 字节。
- SHA-256：`72633b64511b31b97b5cb8f25b2651cca62db96ce86f46a2d8d00b354f9febf1`。
- 使用仓库标准开发测试密钥，保留 debuggable 以便提取日志。

## 安装

将 APK 复制到平板，点击安装；也可以使用本次工作区的 `../build-tools/android-sdk/platform-tools/adb.exe`：

```text
adb install -r OBS-Android-arm64-hwcodec-browser-20261105-2029.apk
```

如系统报告签名不兼容，应先备份旧版配置/推流设置，再处理旧版安装；不要为更新测试包直接删除尚未备份的数据。

## 本次：1080p60 YUV 采集卡推流优化

面向 USB 5 Gbps UVC bulk 采集卡的 YUYV/NV12 → MediaCodec H.264 路径。USB 接收与解析/OBS 交付分线程，预分配缓冲池通过所有权交接重提交；有界过载与序号缺口保护半帧。标准 payload 路径修正 PROBE 帧长/最大 payload 字段偏移，按 USB payload 直接重组 raw YUV；兼容路径改为延迟压实。源属性新增默认开启、仅对明确 YUYV/NV12 生效的“YUV 低延迟推流模式”，保持所选格式/尺寸/帧率并优先最新帧；重协商前停止排干旧泵，新增五秒聚合指标。

请选 YUYV 或 NV12、1920×1080、60 fps，编码器选 MediaCodec H.264。主机收包/取消/过载/重组/完整 1080p 图像、生产模式/PROBE/平面/时间戳回归通过；完整 Android 原生/Qt/Java/APK 构建、签名、16KB 对齐及包内新功能/原生库一致性核查通过。本版没有连接采集卡，不能据此认定 1080p60 吞吐或端到端延迟达标。详细设置、带宽计算、标准/兼容路径与实机步骤见 [专项测试说明](UVC_1080P60_TEST.md)。

## 上版：屏幕源透明合成

屏幕源属性新增“透明度处理”：兼容（默认）、保留 Alpha（直通）、保留 Alpha（预乘输入）。先保持 GPU 开启，选择预乘输入，系统共享授权选择单个应用 VTube Studio；在 OBS 屏幕源下放颜色或图片背景检查透明效果。新接入 Android 14+ 内容尺寸回调，单应用窗口变化不再一直沿用整屏比例；GPU 默认开启、失败重试且不自动切换 CPU。

CPU 转换、投影生命周期/内容尺寸、小窗坐标主机回归与 ANGLE 合成测试通过；完整原生/Qt/Java/APK 构建、签名、ZIP16KB、版本/回调/透明选项入包及原生库一致性检查通过。未连接平板进行本版透明采集验证，不能据此认定 VTube Studio 的实际投影帧已保留 Alpha。设置说明、检查半透明边缘的方法与范围见 [透明采集测试](TRANSPARENT_CAPTURE_TEST.md)。

## 上版：实机小窗与推流修正

已通过 USB 复现旧包错位，并验证新版统一窗口坐标后，全屏转小窗、拖动、缩放与返回全屏的预览位置正确。0.1.9 的模式分支已删除。另修复 RTMP 重连动态码率插值表未重置、MediaCodec CQ 模式仍尝试动态码率，以及发送失败路径的编码包泄漏。提取实际函数的局部回归、完整构建、签名/16KB 对齐与包内容检查通过。本地 RTMP 实测已完成：88.6 秒、5317 帧、60.000 fps；主动断线后约 1.267 秒重连成功；本地 2 Mbps 限速时实际硬编码码率下降，解除后回升。各接收文件均完整解码无错。原配置已逐字节恢复，临时配置和转发已删除。具体统计与范围见 [实机验证记录](DEVICE_VALIDATION_WINDOW_STREAM.md)。本地回环不能替代真实平台、Wi‑Fi 丢包与长期温控测试。

## 上版：小窗预览位置兼容

用户确认 0.1.8 灰块已消失，但小窗出现错位。0.1.9 为标准小窗/分屏模式恢复此前的初始放置契约，不再扣除 Android 父容器屏幕原点；后续位置变化按增量更新，保留初次测量校正，空闲或只改尺寸不重置位置。全屏沿用灰块修正。主机回归、实际 SDK 编译和包检查通过；尚未在设备确认小窗修复。请覆盖安装并重启，检查进入/移动/缩放小窗、从小窗启动、返回全屏与开关源属性。

## 上版：屏幕采集预览底部灰块

0.1.8 修正原生预览的屏幕/父容器坐标换算，并增加每秒一次的活动预览几何核对，处理授权返回和父布局移动后漏同步。稳定布局不重复触发 relayout；不改 GPU 采集和录像通路，GPU 默认开启、失败重试策略保留。实际方法提取的主机回归、完整构建与包校验通过；未连接到设备，按用户要求先交付测试包，灰块是否消失尚待实机确认。安装后重启应用，重新启用屏幕采集，并测试开关源属性、旋转与预览分隔条。详见 [ANDROID_PROJECTION_TEST.md](ANDROID_PROJECTION_TEST.md)。

## 上版实机确认：4K H.264 硬解只显示一角

已通过 USB 在用户平板复现并验证修复。旧版玄戒组件 `c2.xring.avc.decoder` 的实际首帧被解码库解释为 176×144；系统输出格式随后已变为 3840×2160，但缺少的可选裁剪/行跨度字段仍沿用旧值。新版在每次输出格式变化时重置可选几何字段，再解析当前格式，保留已有 YUV 拷贝和转换缓存修复。

同一视频、原有场景变换不变，重新勾选“可用时使用硬件解码”后显示完整画面，关闭并重启应用后再次确认正常。两次日志均为 `component=c2.xring.avc.decoder hardware=true width=3840 height=2160 coded=3840x2160 linesize=3840,1920,1920`，没有以软件回退代替验证。已保留硬解勾选。此结论限于该设备和该 4K H.264 文件，不代表所有格式或长时间录制已验证。

新增 `test-mediacodec-format-reset.py` 覆盖初始 176×144、裁剪 175/143 切到缺少可选字段的 4K 输出。主机裁剪/行跨度、转换缓存回归及完整 APK 构建、签名、16KB 对齐和包内容检查通过。OBS 官方图标已入包，设备桌面图标已确认显示。

## 本次新增：质量控制、媒体源硬解和 GPU 重试

高级输出的 MediaCodec 编码器可选 CBR/VBR/CQ 与自动/低/中/高质量档位，按硬件实际范围映射。屏幕 GPU 默认开启，故障按 2/4/8/16/30 秒退避重试，不自动回退 CPU；仍保留手动取消 GPU 的选项。媒体源修复 content URI 读取并新增按命名硬件组件的 NDK MediaCodec 解码，初始化失败可回退软件；这与屏幕采集策略独立。

需要更换视频时，重新浏览选择并授权，检查声音、画面、定位、循环及实际硬解首帧日志。完整操作与边界见 [MEDIA_QUALITY_TEST.md](MEDIA_QUALITY_TEST.md)。硬解画面已按上文实测；编码质量档位仍待单独实机对比。

## 本次优先验证：HEVC 与可选 GPU 采集

独立录像编码器可选择 Android MediaCodec HEVC (H.265)，使用混合 MP4、1080p60、SDR BT.709/部分范围。简单模式选择独立录像质量；“同推流质量”会继续复用推流编码器。屏幕源的 GPU 开关默认开启，用户已确认可用；失败会退避重试 GPU，不自动回退 CPU，可手动取消用于兼容性对比。授权失效时仍需重新启用并授权。

用 `adb logcat -v threadtime -d -s OBS-MediaCodec:* > mediacodec-log.txt` 收集此次录像的诊断，再运行 `python scripts/verify-mediacodec-log.py mediacodec-log.txt --codec hevc`。需要同会话的实际组件启动、首关键包和持续增长的输出/交付计数，不能只看硬编名称。完整步骤、文件解码检查、GPU 回退条件和性能分析见 [HEVC_HARDWARE_TEST.md](HEVC_HARDWARE_TEST.md)。

HEVC/暂停与积压/音频停止/GPU 回退的主机回归、24 项验证器测试以及完整 Android 原生/Qt/Java/APK 构建通过。签名、ZIP/ELF 16KB 对齐、36 个库依赖闭包、GPU Java 类和原生桥接验证通过；没有连接目标设备，HEVC 驱动与 GPU 采集表现仍需实机测试。

## 保留验证：屏幕采集和 MediaCodec 预设

停用再启用屏幕源，重新授权“整个屏幕”，使用默认“1080p 上限”。如果旧场景中源的大小不合适，执行“变换 → 适应屏幕”。反复切场景、开关源和旋转，确认不再因同一授权重复创建显示而失败，停用后也不会被定时回调自动重开。若仍失败，源属性会直接显示系统异常，反馈完整文字。

简单输出的推流编码器选择 MediaCodec，确认 x264 的 veryfast/ultrafast 等预设及自定义参数隐藏，切回 x264 后原配置仍在。完整回归步骤见 [ANDROID_PROJECTION_TEST.md](ANDROID_PROJECTION_TEST.md)。生命周期与采集尺寸主机回归已通过；完整原生/Qt/Java/APK 构建、签名、16KB 对齐、依赖和新增会话类/接口检查通过，手机上的投影服务与驱动行为仍需实机验证。

## 保留验证：Download/obs

录像路径输入 `/storage/emulated/0/Download/obs`，选择混合 MP4，关闭文件分割，先录制一分钟。确认 Download/obs 中的文件非空、能正常播放且有声音。再完全关闭并重启应用，重复录像。也可通过系统目录选择器授权 Download/obs 子目录。此次新增 MediaStore / SAF 直接写入，修复无权限或 content URI 查询失败被误报为磁盘不足的问题；保留真实低空间保护。

共享目录暂不支持文件分割、回放缓存、自动 remux 或 FFmpeg 自定义输出；文件名不能含子目录。完整测试步骤及边界见 [ANDROID_STORAGE_TEST.md](ANDROID_STORAGE_TEST.md)。本次未连接实机，文件写入、播放和重启后的权限保持仍待设备回归。

## 第一轮：硬编与录制

1. 授予相机、麦克风、通知等实际使用功能所需的权限。确认新版本号。
2. 视频设置：1920×1080、60 fps、SDR BT.709、Limited/部分范围。
3. 输出选择 Android 硬件 H.264（MediaCodec）；简单录制可先用 HQ 档 12 Mbps CBR、Hybrid MP4。若沿用旧配置，手动选择新编码器。
4. 先录制 60 秒，画面中使用持续运动的图案或视频。检查文件可播放、有声音、方向/颜色正常；继续测试推流。
5. 若启动失败，保存日志里的具体 MediaCodec 组件名及错误。能力枚举不等于驱动一定能成功编码，不能把改用 x264 的结果作为硬编测试结果。

## 第二轮：浏览器源

在来源列表添加 Android 浏览器源，输入可信 HTTPS URL。先使用 1280×720、30 fps 导入上限；输出保持 1080p60。测试刷新、CSS、显隐、尺寸修改、场景切换和交互窗口中的点击/拖动。

将 `obs-studio/plugins/android-browser/browser-test.html` 托管到可访问的可信 HTTPS 地址，检查 TOP/BOTTOM 标记、Canvas、WebGL 和按钮；在其后放置彩色源验证真正透明度。网页独立音轨、键盘、滚轮、多点触控和本地 HTML 文件选择器未实现。

## 第三轮：持续性能

分别做“纯采集”“采集+网页”“推流+同编码器录制”60 分钟测试，记录源端帧率、渲染延误、编码跳帧、音画同步、温度与热降频。再测试切到游戏/后台、网络断开重连以及反复启停。降到 30 fps 或重复帧凑够 60 fps 不算通过。

电脑装有 ffprobe/ffmpeg 后，可检查录像：

```text
python scripts/verify-1080p60.py recording.mp4
```

## 反馈日志

```text
adb logcat -d -s ObsCodecCapabilities:* OBS-browser:* OBS-storage:* OBS-screen:*
adb shell run-as com.obsproject.studio ls files/.config/obs-studio/logs
```

按列出的具体文件名读取 OBS 日志，例如：

```text
adb exec-out run-as com.obsproject.studio cat files/.config/obs-studio/logs/<具体日志文件名>
```

反馈时附设备系统版本、场景源、编码组件名、输出参数、问题发生时间、录像样本及相关日志；分享前移除推流密钥、登录信息等私人数据。

## 交付验证结果

完整 CMake 配置、所有原生目标及 Qt 前端链接、Gradle Java/Dex/APK 构建均成功；签名和 ZIP 16KB 对齐验证通过。包内 36 个共享库均为 AArch64，ELF 16KB 对齐且所需动态库可由包内或 Android API 29 公共库满足。新插件、Java 类、关键接口导出与引用检查通过。这些检查不等同于已在目标设备运行，也不构成持续 1080p60 性能认证。
