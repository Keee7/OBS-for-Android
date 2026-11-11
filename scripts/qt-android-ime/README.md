# Qt Android Runtime Patches / Qt Android 运行时补丁说明

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This directory contains targeted patches for Qt Android runtime Java components based on official Qt 6.11.1 sources:
* `QtInputDelegate.java`: [Qt 6.11.1 upstream source](https://github.com/qt/qtbase/blob/v6.11.1/src/android/jar/src/org/qtproject/qt/android/QtInputDelegate.java)
* `QtRootLayout.java`: [Qt 6.11.1 upstream source](https://github.com/qt/qtbase/blob/v6.11.1/src/android/jar/src/org/qtproject/qt/android/QtRootLayout.java)

Original SPDX license headers (LGPL-3.0-only / GPL-2.0-only / GPL-3.0-only) are retained.

### Key Modifications

1. **`QtInputDelegate` Redundant Show Suppression**:
   * Skips redundant Android 11+ `InsetsController.show(ime)` requests when the IME is confirmed visible, focus generation matches, and input hints have not changed.
   * Tracks edit-view popup ownership and automatically dismisses cursor handles when views detach or dialogs close.
2. **`QtRootLayout` Geometry Synchronization**:
   * Registers a Qt application-state listener and synchronizes current attached root dimensions to Qt once native integration is fully initialized, resolving startup layout mismatch issues in split-screen and floating window modes.

### Usage Example

```sh
python scripts/qt-android-ime/patch-qt-android-jar.py \
  --qt-version 6.11.1 --jar "$QT/android_arm64_v8a/jar/Qt6Android.jar" \
  --android-jar "$ANDROID_SDK_ROOT/platforms/android-36/android.jar" \
  --javac "$JAVA_HOME/bin/javac" --output "$BUILD/Qt6Android-patched.jar"
```

The patched jar replaces the staged `libs/Qt6Android.jar` before invoking Gradle during packaging. The installed Qt SDK jar remains untouched.

---

<a name="简体中文"></a>
## 简体中文

本目录包含针对 Qt 6.11.1 Android 运行时 Java 组件的专用补丁：
* `QtInputDelegate.java`：基于 [Qt 6.11.1 官方源码](https://github.com/qt/qtbase/blob/v6.11.1/src/android/jar/src/org/qtproject/qt/android/QtInputDelegate.java)；
* `QtRootLayout.java`：基于 [Qt 6.11.1 官方源码](https://github.com/qt/qtbase/blob/v6.11.1/src/android/jar/src/org/qtproject/qt/android/QtRootLayout.java)。

完整保留上游 SPDX 开源许可证声明（LGPL-3.0 / GPL-2.0 / GPL-3.0）。

### 核心修改点

1. **`QtInputDelegate` 多余弹起请求拦截**：
   * 在 Android 11+ 环境下，当软键盘已被系统确认为可见、当前编辑框焦点与输入选项未变时，跳过多余的 `InsetsController.show(ime)` 调用；
   * 准确跟踪手柄所有权视图，在失焦、对话框关闭或视图解绑时自动清理光标手柄，杜绝残影。
2. **`QtRootLayout` 根布局尺寸同步**：
   * 注册应用状态监听器，在原生初始化完成后主动读取当前实际附加的非零根布局尺寸并上报给 Qt，解决系统分屏与小窗冷启动时的初始几何错位问题。

### 打包应用流程

```sh
python scripts/qt-android-ime/patch-qt-android-jar.py \
  --qt-version 6.11.1 --jar "$QT/android_arm64_v8a/jar/Qt6Android.jar" \
  --android-jar "$ANDROID_SDK_ROOT/platforms/android-36/android.jar" \
  --javac "$JAVA_HOME/bin/javac" --output "$BUILD/Qt6Android-patched.jar"
```

构建脚本在执行 Gradle 打包前，会自动生成已修补的 Jar 包并替换暂存目录下的 `libs/Qt6Android.jar`，本机安装的原生 Qt SDK 目录不会被修改。
