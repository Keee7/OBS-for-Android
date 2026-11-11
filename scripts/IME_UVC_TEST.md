# 输入法与 UVC 优化：版本 25

版本 `0.1.24-ime-uvc-test` / versionCode `25`。前置 PR #4、#5 已合并，当前修复基于主线 `1f81a08`。默认包名仍为 `com.obsproject.studio`，本地测试使用 `-AntutuPackage`。

## 实机旧包基线

Android 16、玄戒 O1 平板，无线 ADB；旧包为版本 24，日志确认运行及编译 Qt 均为 6.9.3。

- 在可取消的场景命名框输入测试文字，五次实际软键盘退格产生四次新的 `SHOW_SOFT_INPUT_BY_INSETS_API` 请求；没有把 ADB 硬件删除键当作软键盘退格。
- 屏幕截图确认文本编辑手柄出现在对话框下方的混音器区域，明显偏离文字框。
- 当前系统自带输入法为 `com.iflytek.inputmethod.miui/.FlyIME`。基线 `mFullscreenMode=false`，因此不能将这次故障仅归因于全屏提取模式。
- UVC 旧包日志显示 NV12、1920×1080、协商 60 fps、SuperSpeed，最近五秒窗口约 59.99 fps、186.76 MB/s；无 USB 错误和队列丢块。这不等于已证明每帧内容唯一。

原始日志及截图仅保存在忽略的 `device-debug/`，不提交用户设备画面和配置。

## 输入法与窗口修复

统一 Windows、WSL 与 CI 的 APK 工具链为 Qt 6.11.1、compile SDK 36、Gradle 9.3.1，target/min SDK 保持 35/29。Windows 使用按 Qt 版本分隔的构建目录，避免 ABI 及 Java 运行时混用。

官方 Qt 的两个版本存在直接相关差异：

- [Qt 6.9.3 QtInputConnection](https://github.com/qt/qtbase/blob/v6.9.3/src/android/jar/src/org/qtproject/qt/android/QtInputConnection.java) 在编辑路径调用 `restartImmInput`；[Qt 6.11.1](https://github.com/qt/qtbase/blob/v6.11.1/src/android/jar/src/org/qtproject/qt/android/QtInputConnection.java) 改为更新提取文本，输入法 Insets 处理也有调整。
- [Qt 6.11.1 Android 输入上下文](https://github.com/qt/qtbase/blob/v6.11.1/src/plugins/platforms/android/qandroidinputcontext.cpp) 的文本手柄坐标路径与旧版不同，避免在对话框中沿用旧的全局坐标转换。

仅升级 Qt 的实机对照包已纠正手柄坐标，但五次真实退格仍产生五次显示请求。调用栈显示系统输入法通过 `sendKeyEvent` 发送退格，随后 Qt 调用 `QtInputDelegate.showSoftwareKeyboard`；未观察到与之对应的 QWidget `RequestSoftwareInputPanel`。补丁在同一个编辑器、同样的输入选项且 Android 确认键盘仍可见时，跳过多余的系统显示请求。保留按键、选区和编辑器更新；首次打开、切换编辑器以及收起后重开仍走 Qt 显示路径。启动时设置 `QT_ANDROID_NO_FULLSCREEN_KEYBOARD=1` 保持普通编辑模式。

主窗口在 Android 活动区域内最大化。对照包记录系统根布局为 3200×2136 物理像素，但 Qt 可用区只有 640×854 逻辑像素（DPR 2.5）。Qt 初始化前只保留第一次可用区域更新，后续尺寸可能丢失。补丁在原生集成就绪时重报当前根布局尺寸，保留随窗口大小变化的原有回调。修改仅作用于打包暂存的 Java 运行时，详见 [补丁说明](qt-android-ime/README.md)。

关闭编辑框后的左上角手柄残影属于已经失焦的 Java 编辑视图。现在按实际拥有手柄的视图清理，覆盖焦点切换、键盘收起和视图移除。复测也捕获了关闭时的 ANR：Android UI 在 `finishComposingText` 等 Qt，Qt 在预览诊断 `rectOnScreen` 等 Android UI。诊断现在直接读布局路径记录的坐标，不再同步调用 Android UI。

实机结果：

- 完整修正版的五次软键盘退格正确删掉五个数字，新增的键盘显示请求为 **0**；光标手柄位于文字下方。收起后再点文字栏发出一次显示请求并正常打开。取消对话框后，原 OBS 进程保持前台，场景列表响应点击，原残影位置没有手柄，同段事件日志没有新增 ANR。
- 全屏主窗和 Qt 可用区均为 **1280×854** 逻辑像素，恢复常规布局。进入 900×640 逻辑像素的小窗后显示紧凑布局；移动和调整尺寸后预览区域及工具栏跟随窗口，随后恢复全屏。800×560 的窄窗仍受主窗 830 逻辑像素最小宽度限制，右侧有轻微裁切。
- 这次验证针对该平板的系统输入法与场景命名框，未覆盖所有 Android ROM、输入法和文字属性控件。

## UVC 实现与验证边界

正常事件循环的显式 mutex 获取由每轮一次停止检查加 32 次逐槽检查，改为一次停止/错误窗口/重试位图交接。错误回调仍在同一把锁下发布状态，恢复期间其他线程触发的新错误留到下一轮；`libusb_clear_halt` 保持在锁外。

保留传输批次、缓冲预算、帧重组、时间戳和设备 quirk。主机回归覆盖正常收包、借用缓冲保护、排干、同步控制线程中的回调、clear-halt 失败和协商。新增 Linux CI 入口 `scripts/test-android-uvc.sh`；Windows 使用 `scripts/test-android-uvc.ps1`。

具有同一 UVC 收包实现的对照包通过无线 ADB 测得 NV12、1920×1080、SuperSpeed。启动时首轮收包有 I/O 错误，软件恢复重开后约 40 秒内累计 2397 帧，稳态五秒窗口约 **59.98–60.00 fps、186.76–186.78 MB/s**，恢复后该区间无 USB 错误或队列丢块，起步阶段记录了 7 个无效帧。接 USB 调试后采集卡未同时连接。此次 HDMI 画面接近静止，不能由相同亮度样本判断内容冻结；锁操作数量也不能直接换算为 CPU 降幅。本轮没有重新测量长时间推流、YUYV 吞吐或重复帧比例。
