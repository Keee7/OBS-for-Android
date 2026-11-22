# Qt Android Runtime Patches / Qt Android 运行时补丁说明

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This directory contains targeted patches for Qt Android runtime Java components based on official Qt 6.12.0 sources:
* `QtInputDelegate.java`: [Qt 6.12.0 upstream source](https://github.com/qt/qtbase/blob/v6.12.0/src/android/jar/src/org/qtproject/qt/android/QtInputDelegate.java)
* `QtRootLayout.java`: [Qt 6.12.0 upstream source](https://github.com/qt/qtbase/blob/v6.12.0/src/android/jar/src/org/qtproject/qt/android/QtRootLayout.java)
* `QtWindowInsetsController.java`: [Qt 6.12.0 upstream source](https://github.com/qt/qtbase/blob/v6.12.0/src/android/jar/src/org/qtproject/qt/android/QtWindowInsetsController.java)

Original SPDX license headers (LGPL-3.0-only / GPL-2.0-only / GPL-3.0-only) are retained.

### Key Modifications

1. **`QtInputDelegate` Redundant Show Suppression**:
   * Skips redundant Android 11+ `InsetsController.show(ime)` requests when the IME is confirmed visible, focus generation matches, and input hints have not changed.
   * Tracks edit-view popup ownership and automatically dismisses cursor handles when views detach or dialogs close.
2. **`QtRootLayout` Geometry Synchronization**:
   * Registers a Qt application-state listener and synchronizes current attached root dimensions to Qt once native integration is fully initialized, resolving startup layout mismatch issues in split-screen and floating window modes.
3. **`QtWindowInsetsController` Phone Landscape Status Bar**:
   * Hides the status bar on landscape phones outside Android multi-window mode. The navigation bar stays visible and a swipe down from the top temporarily reveals the status bar.

### Usage Example

```sh
python scripts/qt-android-ime/patch-qt-android-jar.py \
  --qt-version 6.12.0 --jar "$QT/android_arm64_v8a/jar/Qt6Android.jar" \
  --android-jar "$ANDROID_SDK_ROOT/platforms/android-36/android.jar" \
  --javac "$JAVA_HOME/bin/javac" --output "$BUILD/Qt6Android-patched.jar"
```

The patched jar replaces the staged `libs/Qt6Android.jar` before invoking Gradle during packaging. The installed Qt SDK jar remains untouched.

The Android build also compiles the Qt 6.12.0 platform plugin from a SHA-256-pinned source archive. Its accessibility patch keeps the Qt accessibility bridge enabled and bounds queries that would otherwise block Android's UI thread during EGL window creation. Packaging replaces the staged plugin alongside the jar. [Upstream Qt bug](https://bugreports.qt.io/browse/QTBUG-140674)

Set `OBS_ANDROID_IME_DIAGNOSTICS=1` or start OBS with `--android-ime-diagnostics` to log focus and cursor geometry without recording typed text.

---

<a name="简体中文"></a>
## 简体中文

本目录包含针对 Qt 6.12.0 Android 运行时 Java 组件的专用补丁：
* `QtInputDelegate.java`：基于 [Qt 6.12.0 官方源码](https://github.com/qt/qtbase/blob/v6.12.0/src/android/jar/src/org/qtproject/qt/android/QtInputDelegate.java)；
* `QtRootLayout.java`：基于 [Qt 6.12.0 官方源码](https://github.com/qt/qtbase/blob/v6.12.0/src/android/jar/src/org/qtproject/qt/android/QtRootLayout.java)；
* `QtWindowInsetsController.java`：基于 [Qt 6.12.0 官方源码](https://github.com/qt/qtbase/blob/v6.12.0/src/android/jar/src/org/qtproject/qt/android/QtWindowInsetsController.java)。

完整保留上游 SPDX 开源许可证声明（LGPL-3.0 / GPL-2.0 / GPL-3.0）。

### 核心修改点

1. **`QtInputDelegate` 多余弹起请求拦截**：
   * 在 Android 11+ 环境下，当软键盘已被系统确认为可见、当前编辑框焦点与输入选项未变时，跳过多余的 `InsetsController.show(ime)` 调用；
   * 准确跟踪手柄所有权视图，在失焦、对话框关闭或视图解绑时自动清理光标手柄，杜绝残影。
2. **`QtRootLayout` 根布局尺寸同步**：
   * 注册应用状态监听器，在原生初始化完成后主动读取当前实际附加的非零根布局尺寸并上报给 Qt，解决系统分屏与小窗冷启动时的初始几何错位问题。
3. **`QtWindowInsetsController` 手机横屏状态栏**
   * 手机横屏且未进入分屏或小窗时隐藏状态栏，保留底部导航栏。从屏幕顶部下滑可临时呼出状态栏。

### 打包应用流程

```sh
python scripts/qt-android-ime/patch-qt-android-jar.py \
  --qt-version 6.12.0 --jar "$QT/android_arm64_v8a/jar/Qt6Android.jar" \
  --android-jar "$ANDROID_SDK_ROOT/platforms/android-36/android.jar" \
  --javac "$JAVA_HOME/bin/javac" --output "$BUILD/Qt6Android-patched.jar"
```

构建脚本在执行 Gradle 打包前，会自动生成已修补的 Jar 包并替换暂存目录下的 `libs/Qt6Android.jar`，本机安装的原生 Qt SDK 目录不会被修改。

Android 构建也会从校验过 SHA-256 的源码包编译 Qt 6.12.0 平台插件。补丁保留 Qt 无障碍功能，并为 EGL 创建期间可能阻塞 Android 界面线程的查询设置等待上限。打包时会一并替换暂存的插件。 [Qt 上游问题](https://bugreports.qt.io/browse/QTBUG-140674)

设置 `OBS_ANDROID_IME_DIAGNOSTICS=1` 或用 `--android-ime-diagnostics` 启动 OBS，可记录焦点和光标位置，日志不含输入的文字。
