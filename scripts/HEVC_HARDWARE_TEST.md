# HEVC、GPU 屏幕采集与硬编运行检查

适用版本：0.1.4-hevc-test，versionCode 5。安装包与 SHA-256 见 DEVICE_TEST_QUICKSTART.md。

## 开启 HEVC

设置 → 输出 → 录像，选择独立录像质量和 **Android MediaCodec HEVC (H.265)**。高级输出也可选择 Android Hardware HEVC。录像格式先用混合 MP4，保留 Download/obs 路径；视频设置使用 1920×1080、60 fps、SDR BT.709、部分范围。简单模式“同推流质量”会复用推流编码器，单独选择录像 HEVC 时请使用高质量等独立录像档。

本版实现 Main / Main 兼容、8-bit 4:2:0、CBR、无 B 帧的 Surface 输入。未实现 Main10/HDR。能力查询检查硬件标记、Surface、尺寸/帧率、Main profile、码率范围与 CBR；启动时按查到的组件名实例化，不把软件编码器当成硬编回退。驱动仍可能在真实配置/启动时失败，届时会停止并记录错误。

默认仍保留 H.264，避免自动改变原有推流配置。推流沿用 OBS 的服务/编码格式过滤；服务能力未知或显示 HEVC 选项不代表服务器一定接收 HEVC，先确认平台及协议支持，再测试。HEVC 录像正常不等于 HEVC 推流兼容。

简单录制沿用 8/12 Mbps CBR，便于首次对比；没有假定 HEVC 在该设备上一定能以固定比例降低码率或功耗。Main/兼容位及位深检查是参数集的有限解析，不替代完整码流一致性测试。

## 可选 GPU 屏幕采集

打开 **屏幕采集源 → 属性 → GPU 屏幕采集（实验，兼容时启用）**。该选项默认关闭，按源保存，与“1080p 上限 / 原生分辨率”独立。

关闭时使用原来的 AImageReader RGBA → CPU 帧 → GPU 上传；开启后使用同一投影授权的 SurfaceTexture → OES 纹理 → OBS 合成，不经 CPU 整帧读取和上传。仍有 GPU 格式转换和同步，不能称整条链路绝对零拷贝，也没有实机性能提升百分比。

属性页出现“实验性 GPU 采集 / 已收到帧”，并在运动画面下“已导入”计数增长，才表示 GPU 路径实际收到画面。若出现“GPU 不可用，已回退 CPU”，当前运行的是兼容路径。创建、更新错误或 10 秒没有首帧时尝试回退；已有首帧后的静止画面不会按此规则判故障。授权已失效时需要重新启用源并重新授权，不能绕过系统同意。

先保留关闭状态测试，再开启测试相同场景，比较渲染延误、编码跳帧、音画同步、CPU 与温度。检查方向、比例、颜色、横竖屏、切后台、停止共享以及反复切换开关。画面大小变化后可执行“变换 → 适应屏幕”。异常时取消勾选；源属性状态是打开时的快照，可重新打开查看。

## 判断硬编是否实际工作

低 CPU 占用、下拉框选中了 MediaCodec、能力列表显示 eligible=true，都不能单独证明编码正在正常输出。

1. 开始持续运动画面的录像，保持 30–60 秒后正常停止。
2. 用电脑连接 ADB，读取专用标签。工作区已有 `../build-tools/android-sdk/platform-tools/adb.exe`，可将其目录加入当前终端 PATH：

```text
adb logcat -v threadtime -d -s OBS-MediaCodec:* > mediacodec-log.txt
python scripts/verify-mediacodec-log.py mediacodec-log.txt --codec hevc
```

也可以在开始录像前运行 `adb logcat -v threadtime -s OBS-MediaCodec:* > mediacodec-log.txt`，停止录像后按 Ctrl+C，避免日志环形缓冲覆盖。不要清空其他调试日志。工具支持普通 UTF-8、UTF-8 BOM 和 Windows PowerShell 5 重定向的 UTF-16 LE BOM。

应在**同一个 session**看到以下证据：

| 事件/字段 | 含义 |
| --- | --- |
| `event=started codec=hevc component=… hardware=true software=false input=Surface` | 已按 Android 报告为硬件的组件名完成 configure、输入 Surface 和 start；不是仅枚举候选 |
| `event=first-packet headers=valid keyframe=1` | 收到可用参数集及首个关键包 |
| `event=progress` 的 `submitted/output/delivered/bytes` 持续增长 | 纹理提交、编码输出及交给 OBS 的包都在前进 |
| `event=summary failed=0`，没有同 session 的 `event=error` | 停止时没有记录编码会话错误 |

脚本默认检查最新匹配会话，失败时不偷偷挑旧的成功记录；多个编码器并行时可用 `--session <id>` 明确选择。它报告快照区间的输出帧率，至少要求两次计数增长的快照，不把单独的启动日志当作持续输出证据。OBS 日志文件也含相同事件；较长会话的重复进度行可能被 OBS 日志限流，专用 logcat 标签更适合完整采集。

这证明使用了 Android 标记为硬件的命名组件并实际产包，不能独立审计厂商内部实现，也不证明文件封装成功、输出恰好 60 fps 或没有冻结。还需检查录像文件和 OBS 统计。

## 检查录像文件与 1080p60

从 Download/obs 取出录像，电脑安装 ffprobe/ffmpeg 后执行：

```text
ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,profile,pix_fmt,width,height,avg_frame_rate -of default=noprint_wrappers=1 recording.mp4
ffmpeg -v error -i recording.mp4 -map 0:v:0 -f null -
python scripts/verify-1080p60.py recording.mp4 --codec hevc
```

应看到 `codec_name=hevc`、Main 兼容的 8-bit 4:2:0、1920×1080 和接近 60 的声明帧率，并能完整解码。性能脚本另检查解码帧数、时间戳间隙、冻结及重复帧；应使用连续运动测试场景，静态画面不能证明运动流畅性。仅显示 HEVC 格式不能证明是硬编，必须与上述同次会话日志结合。

在 OBS 统计窗口观察实际帧率、渲染延误和编码跳帧；再做 30–60 分钟持续录像/推流，检查温度和降频。若视频采集本来不够 60 帧、只是重复图像凑成 60 fps，不能作为持续性能通过。

## 已落地优化与后续空间

| 项目 | 本版处理 / 后续方向 |
| --- | --- |
| 编码包复制 | 普通访问单元由两次分配/复制改成一次，并将同一缓冲区移交队列；首帧只提取 VPS/SPS/PPS 等参数集到额外数据 |
| 分配与枚举 | 缓存限定数量的硬件能力查询；浏览器复用 GL 状态查询数组，减少逐帧小分配 |
| 有界积压 | 编码输出队列同时受 120 包 / 32 MiB 限制，单访问单元 16 MiB；待编码输入按约 5 秒、最低 120 帧设上限，避免驱动缓慢输出时核心时间戳记录无限增长 |
| 无输出 | 连续提交时 5 秒没有视频输出判异常；空闲暂停后的新输入有新的等待窗口。它不能强行打断厂商驱动永久卡在 EGL/MediaCodec 系统调用中的情形 |
| 音频停止 | 拆分生命周期锁与 sink 锁，避免持 sink 锁 join 正在等待该锁的音频线程；有会触发旧死锁的负对照测试 |
| 屏幕带宽 | 保留默认 Full HD 上限，并增加上述可选 GPU 通路；真实吞吐、颜色和同步仍待玄戒实测 |
| 后续优先级 | 在设备上分段量取采集、合成、Surface 提交等待与写盘耗时；据瓶颈调节采集尺寸、网页导入帧率、隐藏网页生命周期。保留跨 EGL 上下文 fence，不能通过删除同步来换表面帧率 |

## 验证边界

已运行 AVC/HEVC 参数与码流转换、首帧混合参数集提取、暂停/无输出/慢输出积压、音频泵死锁和 GPU 首帧回退的主机回归；日志与文件验证器使用合成数据的 24 项测试通过。完整 Android 原生、Qt、Java/Dex/APK 编译、签名和包验证另外执行。

本地没有连接目标 Android 设备，也没有设备录制的 HEVC 样本；GPU 模式和真实 HEVC 编码性能仍待实机验收。编码能力判断依据见 [Android MediaCodecInfo](https://developer.android.com/reference/android/media/MediaCodecInfo)，CSD 参数集布局见 [Android MediaCodec](https://developer.android.com/reference/android/media/MediaCodec)。
