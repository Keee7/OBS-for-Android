# 软件编码、图像 URI 与输入法定位

源码版本：`0.1.23-softcodec-image-test` / versionCode `24`。本轮不连接 ADB，不做实机测试。整体目标为稳定吞吐、低 CPU/GPU 和合理内存占用，允许适当缓冲。UVC 生产代码未改动，审查结论见 [UVC_CPU_REVIEW.md](UVC_CPU_REVIEW.md)。

## APK 与构建验证

已生成 `releases/OBS-Android-arm64-obs-hwcodec-browser-20261110-020745.apk`，120,778,782 字节，包名 `com.obsproject.studio`，使用用户指定的仓库外 release 签名。SHA-256：`e80397d88c3f0150bc5d67e26bdb48e09e837b7c4d0213c0e81281413880833e`。

Windows Qt 6.9.3 全量 configure、native/Qt/Java 构建、签名（v3、单签名者）、16 KiB zipalign、严格 APK 校验均通过。核验包含实际 FFmpeg x265/AOM 编码器描述符、图像 URI 读取代码，以及包内项目二进制与本次编译输出的一致性。未安装或启动测试。

## H.265 与 AV1 软件编码

- 增加 `ffmpeg_x265`（Software HEVC / x265）并接入已有 `ffmpeg_aom_av1`（AOM AV1）。依赖固定为 x265 3.6、libaom 3.10.0、FFmpeg 7.1.3；两个构建路径均显式开启库支持，运行时按实际编码器存在与否注册。
- 简单输出可选择软件 HEVC/AV1；推流列表同时遵守服务支持的编码格式。高级输出提供码率控制、速度和线程选项。x265 的界面 CRF 内部沿用 OBS 的 `CQP` 设置键，实际传给 x265 的是 CRF，非固定 QP。
- x265 默认为 ultrafast、8-bit SDR I420；AOM Android 默认为 realtime 算法、速度 8，启用行并行。FFmpeg 7.1.3 的 `cpu-used` 上限为 8，因此 Android 不显示 9/10，并检查速度设置是否成功，避免被拒绝后静默保留较慢的默认速度 1。默认自动线程可手工限制，避免软件编码占满 CPU。MediaCodec 仍适合低 CPU 占用推流；没有确认软编可持续达到 1080p60。
- 自定义 FFmpeg 选项保留上游行为，部分无效项仅产生警告，尤其 x265 的内部 `x265-params`；本轮没有保证任意自定义字串都会生效，应以日志为准。内建 x265 档位和 AOM 速度/并行选项单独检查返回值。
- 保留 x265 ARM64 汇编；修复上游自定义汇编命令缺 Android target，以及 NDK r30 下 pkg-config 私有链接项。静态 FFmpeg 携带 x265/AOM/C++ runtime 及 postproc 依赖。
- 公共编码辅助函数在发送 EAGAIN 时先接收旧包，再重试同一输入帧；异常时明确失败，避免静默丢帧。复用帧前调用 `av_frame_make_writable`。OBS 的 PTS 已按 fps_den 递增，时间转换没有重复乘分子。

已完成：NDK 语法检查、实际 Android 依赖编译、`obs-ffmpeg` 原生目标链接；主机 `ffmpeg-submit-test` 覆盖背压、异常清理和 60000/1001、30000/1001 时间转换。尚未完成设备初始化、实际输出解码、长期软编吞吐和温控验证。

## 图像选择器的 content URI

用户反馈通过“浏览”选择后得到 `content://`，图像源不显示。原图像路径直接交给 FFmpeg 文件输入和 GIF 文件判断，未接入 Android 文档读取。

- `content://` / `file://` 通过已有 Android 存储桥接取得授权 FD；静态图像使用自定义 AVIO，按内容探测格式，不要求 URI 有扩展名，不先复制整份文档到缓存。
- GIF 检查支持 URI 签名；修复空文件句柄、seek 失败和失败初始化清理。静态图像解码正确消耗输入包与 EOF，避免重复投喂同一包。
- URI 不进入普通文件系统 stat/缺失文件检查。手动更新源会重新读取；当前不定时查询文档修改时间。
- 本地可 seek 的提供者是支持范围。云端/管道 FD、被撤销权限及跨应用重启权限保留，仍需在 Android 上检查；现有存储桥接会拒绝不可 seek 的 FD。

已完成主机检查：PNG（含透明度逐像素比对）、JPEG、GIF、无扩展名 content URI、file URL、本地路径、损坏/截断输入、拒绝读取、分配失败、seek 与释放。测试从实际生产 C 函数构建，桥接仅用本地 FD 替代 Android 提供者；不代表 Android 选择器或 GPU 渲染已实测。

```powershell
# 需要匹配的 FFmpeg 主机开发库、C 编译器和 Pillow；Windows 用 MSYS2 UCRT64 GCC。
python scripts/test-image-document.py --ffmpeg-prefix <host-ffmpeg-prefix>
```

## 输入法删除与光标偏移：仍未解决

已知现象：所有属性文字栏、系统输入法、英文/数字删除也复现，普通输入正常。尚未有证据证明所有文字栏都在删除时重建，也未确认是哪一层错误触发 IME restart。不会用吞掉删除事件或硬编码坐标补偿代替定位。

增加默认关闭的 `OBS_ANDROID_IME_DIAGNOSTICS=1`，以及 Android 启动参数 `--android-ime-diagnostics`。只记录焦点对象、删除事件、输入长度、替换范围和编辑器/窗口/屏幕坐标，不记录输入正文、属性名或路径。关闭时不安装事件过滤器，不创建采样计时器；开启时不拦截事件、不改变焦点。

后续设备诊断步骤（本轮未执行；先停止输出再重启）：

```sh
adb shell am force-stop com.obsproject.studio
adb shell am start -n com.obsproject.studio/org.qtproject.qt.android.bindings.QtActivity --es applicationArguments --android-ime-diagnostics
adb logcat -v threadtime > ime-log.txt
```

重现一次正常输入和删除，保存包含 `OBS-IME` 的日志与同时间段 Qt/Android 输入法日志。重新正常启动应用即可关闭这次诊断。Antutu 包需替换包名，Activity 类名不变。

`applicationArguments` 转发依据 [Qt 6.9.3 QtLoader](https://github.com/qt/qtbase/blob/v6.9.3/src/android/jar/src/org/qtproject/qt/android/QtLoader.java)。本地主机构建使用 Qt 6.9.3；CI 使用 Qt 6.11.1，应分别确认行为。新增 Qt Widgets 独立检查 `scripts/ime-diagnostics-test` 检查诊断开/关时输入、删除、焦点对象与日志内容，不代表 Android IME 问题已复现或修复。
