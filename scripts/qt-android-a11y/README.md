# Qt Android Accessibility Deadlock Fix / Qt Android 无障碍死锁修复

[English](#english) | [简体中文](#简体中文)

> **Prebuilt test build / 预构建测试包**: [v0.2.1-a11y-deadlock-fix](https://github.com/shiyidzyk/OBS-for-Android/releases/tag/v0.2.1-a11y-deadlock-fix)
> (arm64-v8a APK + this patch, built and verified locally; debug-signed, uninstall the store version first).
> 上游 PR: [Keee7/OBS-for-Android#12](https://github.com/Keee7/OBS-for-Android/pull/12)

---

<a name="english"></a>
## English

### Symptom

With an accessibility service enabled (`accessibility_enabled=1`), the Android build
aborts with `signal 6` as soon as a second top-level window is created — opening the
settings dialog, opening "add source", opening any source properties dialog, or opening
the hamburger menu. Logcat shows:

```
W Qt A11Y : Failed to acquire deadlock protector for 'QAndroidPlatformOpenGLWindow::eglSurface()'
            while already locked by 'QtAndroidAccessibility::runInObjectContext().'
F Qt      : Failed to acquire deadlock protector for QAndroidPlatformOpenGLWindow::eglSurface().
```

The two lines are followed by `qFatal` → `abort()`. Disabling the accessibility service
makes the same actions work, which is why the issue only shows up for users of
TalkBack-like tools and for accessibility inspection tools.

### Root cause

`QtAndroidPrivate::AndroidDeadlockProtector` (`qtbase/src/corelib/kernel/qjnihelpers.cpp`)
is a **process-wide, non-reentrant** atomic flag. In Qt 6.11.x only two places take it:

| Site | Behaviour when the flag is already taken |
| --- | --- |
| `qandroidplatformopenglwindow.cpp::eglSurface()` | `qFatal(...)` → `abort()` |
| `androidjniaccessibility.cpp::runInObjectContext()` | warning, access is dropped |

The accessibility bridge runs on the Android UI thread, takes the protector, and then
performs a `Qt::BlockingQueuedConnection` call into the Qt main thread. While that
blocking call is outstanding the Qt main thread shows a new window, which needs a new
`EGLSurface`, i.e. `eglSurface()`, i.e. the same protector. The protector is not
reentrant, the Qt main thread cannot acquire it, and Qt treats that as a fatal error and
aborts the process. The reverse order is harmless: if the Qt thread holds the flag first,
Android accessibility queries are simply dropped.

Qt 6.12 reworked `QAndroidPlatformOpenGLWindow` (no protector, no surface wait condition,
`qWarning` instead of `qFatal`), so 6.12 is unaffected. 6.11.1/6.11.3 still abort.

### Fix

`qt-6.11.1-a11y-deadlock.patch` contains two complementary changes:

1. **`androidjniaccessibility.cpp` / `runInObjectContext()`** — no unbounded blocking:
   * If the call arrives on the object's own thread it is dispatched directly; the
     non-reentrant protector is not taken at all (the query can itself create/resize a
     window and would need that same protector).
   * Otherwise the call is posted with a normal `Qt::QueuedConnection` and waited for
     with a **bounded** `QSemaphore::tryAcquire(1, 120 ms)`. The state handed to the
     queued lambda lives in a `std::shared_ptr`, so a timeout can never write to a dead
     stack frame. On timeout the query is dropped with a warning — exactly the behaviour
     upstream already had when the protector was busy.
2. **`qandroidplatformopenglwindow.cpp` / `eglSurface()`** — no `qFatal`:
   * The surface is created only after the protector is acquired, but acquisition is
     **retried** (25 ms × 24 ≈ 600 ms) instead of aborting. Since the accessibility
     bridge now gives up after 120 ms, the Qt thread always wins eventually and
     `createSurface()` + the surface wait run with the Android UI thread already free.
   * If the protector is still busy after the retry window, Qt logs a warning, returns
     the current surface and posts an expose event so a later frame retries the creation.

Result: no abort, no deadlock, no ANR. The only trade-off is that an accessibility query
that cannot be answered within 120 ms is dropped (`Qt A11Y : ... timed out, dropping the
request`), which degrades screen-reader metadata slightly while a window is being created.

### Build

Only the Android platform plugin has to be rebuilt — the rest of the Qt libraries inside
the APK stay untouched and ABI-compatible:

```powershell
# 1. qtbase 6.11.1 sources (same version as the Qt used to build the app)
#    https://download.qt.io/official_releases/qt/6.11/6.11.1/submodules/qtbase-everywhere-src-6.11.1.tar.xz

# 2. patch + build the plugin
pwsh -File scripts/qt-android-a11y/build-patched-qt-android-plugin.ps1 `
  -QtSrc D:\qtbase-everywhere-src-6.11.1 `
  -BuildDir D:\qtbase-build `
  -QtHostPath D:\Qt\6.11.1\msvc2022_64 `
  -Ndk D:\android-ndk-r30 `
  -AndroidSdk D:\Android\SDK `
  -OutSo D:\plugin-patched\libplugins_platforms_qtforandroid_arm64-v8a.so
```

`git apply`-equivalent manual steps:

```sh
git -C "$QTBASE_SRC" apply scripts/qt-android-a11y/qt-6.11.1-a11y-deadlock.patch
cmake -S "$QTBASE_SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_MAKE_PROGRAM=ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
  -DANDROID_SDK_ROOT="$ANDROID_SDK" -DQT_HOST_PATH="$QT_HOST" \
  -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF -DFEATURE_accessibility=ON
ninja -C "$BUILD" libplugins_platforms_qtforandroid_arm64-v8a.so
"$NDK/toolchains/llvm/prebuilt/<host>/bin/llvm-strip" --strip-unneeded \
  "$BUILD/plugins/platforms/libplugins_platforms_qtforandroid_arm64-v8a.so" -o patched.so
```

Then replace `lib/arm64-v8a/libplugins_platforms_qtforandroid_arm64-v8a.so` in the APK
(keep `resources.arsc` stored uncompressed, re-run `zipalign -f 4` and `apksigner`) and
install with `adb install -r -t`.

Note: `-DANDROID_SDK_ROOT` is mandatory for Qt's `QtPlatformAndroid.cmake`; omitting it
fails configure. The plugin must be built from the exact Qt version that produced
`libQt6Gui_arm64-v8a.so`, otherwise the loader rejects it.

### Verified

Verified on the Android build of this repository (v0.2.1, Qt 6.11.1, arm64-v8a), on an
emulator (Android 15, x86_64/arm64 translation, same failure as on a physical device)
with `accessibility_enabled=1` and two active accessibility services:

| Scenario | Before | After |
| --- | --- | --- |
| Open settings | `qFatal` / `signal 6` | renders, stays alive |
| Add source → colour → new colour | abort | renders |
| Source properties dialog | abort | renders, source added to the scene |
| Hamburger menu | abort | opens |
| 22 × open/close settings, 6 × add-source cycles | abort on the first/second | no abort, no ANR, single pid throughout |

`logcat` after the patched run contains no `FATAL`, no `ANR in`, no
`Failed to acquire deadlock protector`, only the expected
`Qt A11Y : Accessibility call to the main thread timed out, dropping the request`
warnings (and none of them once the UI is idle).

A ready-to-install build of this fix (base repo v0.2.1 + patched plugin, arm64-v8a,
debug-signed) is attached to
[release `v0.2.1-a11y-deadlock-fix`](https://github.com/shiyidzyk/OBS-for-Android/releases/tag/v0.2.1-a11y-deadlock-fix);
the branch used for verification is
[`fix/android-a11y-deadlock`](https://github.com/shiyidzyk/OBS-for-Android/tree/fix/android-a11y-deadlock).

---

<a name="简体中文"></a>
## 简体中文

### 现象

开启无障碍服务（`accessibility_enabled=1`）后，只要创建第二个顶层窗口就必然闪退
（`signal 6`）：打开"设置"、打开"添加源"、打开任意源属性对话框、打开右上角菜单，都会
触发。logcat 关键两行：

```
W Qt A11Y : Failed to acquire deadlock protector for 'QAndroidPlatformOpenGLWindow::eglSurface()'
            while already locked by 'QtAndroidAccessibility::runInObjectContext().'
F Qt      : Failed to acquire deadlock protector for QAndroidPlatformOpenGLWindow::eglSurface().
```

随后 `qFatal` → `abort()`。关闭无障碍服务后同样的操作完全正常，因此该问题只在
TalkBack 类读屏工具与无障碍调试工具开启时出现。

### 根因

`QtAndroidPrivate::AndroidDeadlockProtector`（`qtbase/src/corelib/kernel/qjnihelpers.cpp`）
是**进程级、不可重入**的原子标志。Qt 6.11.x 中只有两处会去抢它：

| 位置 | 已被占用时的行为 |
| --- | --- |
| `qandroidplatformopenglwindow.cpp::eglSurface()` | `qFatal(...)` → `abort()` |
| `androidjniaccessibility.cpp::runInObjectContext()` | 打警告并丢弃本次访问 |

无障碍桥在 Android UI 线程上先抢到标志，然后用 `Qt::BlockingQueuedConnection` 阻塞调用
Qt 主线程；而在阻塞期间 Qt 主线程正好要显示新窗口，需要新的 `EGLSurface`，即调用
`eglSurface()`，也就是同一个标志。标志不可重入，Qt 主线程抢不到，Qt 把它当成致命错误
直接 abort。反过来（Qt 线程先抢到）则无害：无障碍查询被安全丢弃。

Qt 6.12 已重写 `QAndroidPlatformOpenGLWindow`（去掉 protector 与 surface 等待，把
`qFatal` 改为 `qWarning`），所以 6.12 没有此问题，6.11.1/6.11.3 仍会崩溃。

### 修复

`qt-6.11.1-a11y-deadlock.patch` 包含两处互补改动：

1. **`androidjniaccessibility.cpp` / `runInObjectContext()`** —— 不做无界阻塞：
   * 若调用本身就在目标对象所在线程，直接同步执行，完全不碰不可重入的 protector
     （无障碍查询本身可能创建/调整窗口，也需要同一把锁）；
   * 否则用普通 `Qt::QueuedConnection` 投递，并用**有界** `QSemaphore::tryAcquire(1, 120 ms)`
     等待。投递给 lambda 的状态放在 `std::shared_ptr` 里，超时也不会写到已退栈的地址。
     超时即丢弃本次查询并打警告 —— 与上游"抢不到锁就丢弃"的既有语义一致。
2. **`qandroidplatformopenglwindow.cpp` / `eglSurface()`** —— 不再 `qFatal`：
   * 仍然只在拿到 protector 之后才创建 Surface，但改为**有界重试**（25 ms × 24 ≈ 600 ms）。
     由于无障碍桥 120 ms 后必定放弃，Qt 线程最终一定拿到锁，`createSurface()` 与
     等待 Surface 时 Android UI 线程已经空闲；
   * 重试窗口内仍拿不到锁时只打警告、返回当前 Surface，并补发一次 expose 事件，
     让后续帧重新尝试创建，而不是留下永不绘制的窗口。

结果：不 abort、不死锁、不 ANR。唯一代价是 120 ms 内拿不到结果的单次无障碍查询会被丢弃
（`Qt A11Y : ... timed out, dropping the request`），即在窗口创建瞬间读屏信息略有缺失。

### 构建

只需重编 Android 平台插件，APK 内其余 Qt 库保持不动且 ABI 兼容：

```powershell
# 1. 取 qtbase 6.11.1 源码（必须与构建 App 所用 Qt 版本一致）
#    https://download.qt.io/official_releases/qt/6.11/6.11.1/submodules/qtbase-everywhere-src-6.11.1.tar.xz

# 2. 打补丁并编译插件
pwsh -File scripts/qt-android-a11y/build-patched-qt-android-plugin.ps1 `
  -QtSrc D:\qtbase-everywhere-src-6.11.1 `
  -BuildDir D:\qtbase-build `
  -QtHostPath D:\Qt\6.11.1\msvc2022_64 `
  -Ndk D:\android-ndk-r30 `
  -AndroidSdk D:\Android\SDK `
  -OutSo D:\plugin-patched\libplugins_platforms_qtforandroid_arm64-v8a.so
```

手工等价步骤（`git apply` + cmake + ninja）见英文小节命令。随后把
`lib/arm64-v8a/libplugins_platforms_qtforandroid_arm64-v8a.so` 替换进 APK
（`resources.arsc` 必须保持不压缩存储，重跑 `zipalign -f 4` 与 `apksigner`），
`adb install -r -t` 安装。

注意：Qt 的 `QtPlatformAndroid.cmake` 必须提供 `-DANDROID_SDK_ROOT`，否则 configure 失败；
插件必须用产出 `libQt6Gui_arm64-v8a.so` 的同一 Qt 版本编译，否则加载器会拒绝。

### 已验证

在本仓库 Android 版本（v0.2.1、Qt 6.11.1、arm64-v8a）、Android 15 模拟器（x86_64/arm64
转译，与真机复现一致）上，开启 `accessibility_enabled=1` 并同时启用两个无障碍服务后实测：

| 场景 | 修复前 | 修复后 |
| --- | --- | --- |
| 打开设置 | `qFatal` / `signal 6` | 正常渲染，进程存活 |
| 添加源 → 颜色 → 新建颜色 | 闪退 | 正常渲染 |
| 源属性对话框 | 闪退 | 正常渲染，源已加入场景 |
| 右上角菜单 | 闪退 | 正常展开 |
| 连续 22 次开关设置 + 6 轮添加源 | 第 1~2 次即闪退 | 全程无闪退、无 ANR，pid 不变 |

修复后 `logcat` 无 `FATAL`、无 `ANR in`、无 `Failed to acquire deadlock protector`，
仅有预期的 `Qt A11Y : Accessibility call to the main thread timed out, dropping the request`
警告（UI 空闲后不再出现）。

可直接安装的验证包（上游 v0.2.1 + 已打补丁的平台插件，arm64-v8a，debug 签名，
需先卸载官方版本）见
[Release `v0.2.1-a11y-deadlock-fix`](https://github.com/shiyidzyk/OBS-for-Android/releases/tag/v0.2.1-a11y-deadlock-fix)；
验证所用分支为
[`fix/android-a11y-deadlock`](https://github.com/shiyidzyk/OBS-for-Android/tree/fix/android-a11y-deadlock)。
