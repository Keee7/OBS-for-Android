# Qt 6.11.1 Android runtime patches

These two Java sources are based on Qt's official 6.11.1 sources:
[QtInputDelegate.java](https://github.com/qt/qtbase/blob/v6.11.1/src/android/jar/src/org/qtproject/qt/android/QtInputDelegate.java)
and [QtRootLayout.java](https://github.com/qt/qtbase/blob/v6.11.1/src/android/jar/src/org/qtproject/qt/android/QtRootLayout.java).
They retain the upstream SPDX license headers (Qt commercial license or LGPL-3.0-only or GPL-2.0-only or GPL-3.0-only). The accompanying `.patch` files show the changes against those sources.

- `QtInputDelegate` skips a repeated Android 11+ `InsetsController.show(ime)` only when Android reports the IME visible, the Java edit view still has focus, the Qt focus generation and edit view match the last show, and input hints and enter-key type match. The app's `ObsImeFocus` bridge tracks Qt focus changes, including different QWidget editors hosted by one QtWindow. Options, layout, focus requests, selection updates, and any required input restart remain in Qt's path. A hidden keyboard, a different editor or options, and the first show still use Qt's original show path. A queued request from an old focus generation is never suppressed by the guard.
- The delegate remembers which edit view actually owns the cursor/selection popups. It clears that view on a hidden-handle request, keyboard dismissal, a change of edit view or view detachment. This prevents a popup from being stranded on an old view after the current edit view has changed. The cleanup runs on the Android UI thread, and queued show updates from an old Qt focus generation are ignored.
- `QtRootLayout` registers a Qt application-state listener while attached. Once the native integration is ready it posts a read of the current nonzero attached root size to the Android UI thread and reports it to Qt. It also handles reattachment after integration is ready and unregisters on detach. The existing size-change path remains; there is no screen-size constant or polling. This addresses a startup ordering where multiple pre-integration layout sizes can leave the first size in Qt's available geometry.

`patch-qt-android-jar.py` accepts only version `6.11.1` and verifies the SHA-256 of both original Qt class files before compiling the replacement classes with JDK 17 and the Android SDK. It writes a separate jar and refuses to overwrite the input; the installed Qt SDK jar is never modified. Package integration must stage Qt dependencies, create the separate patched jar, replace the **staged** `libs/Qt6Android.jar`, and then invoke Gradle. Running `androiddeployqt` again after replacement can overwrite the staged jar. The app Java package must include `ObsImeFocus.java` and native OBS must connect its focus-change bridge. A newer Qt version requires a fresh source and behavior review, updated patch, class hashes, and tests; do not bypass the version or hash checks.

Example (paths are illustrative):

```sh
python scripts/qt-android-ime/patch-qt-android-jar.py \
  --qt-version 6.11.1 --jar "$QT/android_arm64_v8a/jar/Qt6Android.jar" \
  --android-jar "$ANDROID_SDK_ROOT/platforms/android-36/android.jar" \
  --javac "$JAVA_HOME/bin/javac" --output "$BUILD/Qt6Android-patched.jar"
# Copy the output to the androiddeployqt staging directory's libs/Qt6Android.jar,
# then run Gradle against that staging directory.
```

The host policy test can be run with JDK 17:

```sh
mkdir -p /tmp/obs-ime-test
"$JAVA_HOME/bin/javac" -d /tmp/obs-ime-test \
  obs-studio/frontend/cmake/android/java/com/obsproject/studio/ObsImeFocus.java \
  scripts/qt-android-ime/ObsImeFocusTest.java
"$JAVA_HOME/bin/java" -ea -cp /tmp/obs-ime-test ObsImeFocusTest
```

This checks the duplicate-show decision, not Android input delivery. The jar script's successful compilation and APK runtime verification establish packaging, not device behavior. On a device, verify initial open, repeated real soft-keyboard deletions and text input, editor switching, dismiss/reopen, input-option changes, and root resizing in fullscreen and an Android small window. Compare actual `onRequestShow` counts, selection/cursor position, and the available geometry with the root view; do not infer IME or layout success solely from host tests.
