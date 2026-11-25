# Download and Install

OBS for Android distributes prebuilt APKs via GitHub Releases, or you can compile it from source.

## Option 1: Direct APK Install (Recommended)

1. Open the project's [GitHub Releases](https://github.com/Keee7/OBS-for-Android/releases) page and download the latest `arm64-v8a` `.apk` build.
2. Tap the APK file in your device's file manager or browser.
3. Grant the "Install unknown apps" permission when prompted by Android to complete installation.

## Option 2: Install via ADB from a Computer

If **Developer options → USB debugging** is enabled, you can install or upgrade over an existing installation while keeping your scenes and settings:

```sh
adb install -r path/to/OBS-Android-*.apk
```

## In-App Update Checks

By default, the app checks GitHub Releases for newer versions on startup:
* **Toggle automatic checks**: Go to **Settings → General → Updates → Automatically check for updates on startup**.
* **Manual check**: Tap `☰` in the Controls dock → **Help (H) → Check for Updates**. If a newer release is available, a prompt lets you jump straight to the release page.

## Building from Source (Developers)

To modify source code or customize plugins, see [BUILDING.md](https://github.com/Keee7/OBS-for-Android/blob/main/BUILDING.md) in the repository. GitHub Actions, Ubuntu/WSL2, and native Windows builds are supported.
