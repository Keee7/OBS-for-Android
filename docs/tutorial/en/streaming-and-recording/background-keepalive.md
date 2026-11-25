# Background Keep-Alive Guide

During mobile game streaming or screen recording, OBS runs in the background most of the time. While OBS includes a native foreground service (`ObsForegroundService`), strict vendor battery managers still require a few quick OS tweaks for multi-hour stability.

## 1. How the Foreground Service Works

Whenever any of the following is active, OBS starts a persistent foreground service notification:
* Active live stream or local recording;
* An active **Screen Capture**, **Built-in Camera**, or **AAudio** capture session.

The service declares `specialUse`, `mediaProjection`, `camera`, and `microphone` foreground service types for Android 14+ compliance and holds a `WAKE_LOCK` to prevent CPU sleep from stalling your stream.

## 2. Essential OS Whitelist Steps

Before going live for a long session, verify these four settings:

1. **Allow Notifications (Critical)**
   * Make sure OBS is allowed to post notifications. Without notification permission, Android blocks the foreground notification and may kill the background process within a minute.
2. **Disable Battery Restrictions**
   * **Xiaomi HyperOS / MIUI**: App Info → Battery saver → **No restrictions**.
   * **OPPO ColorOS / OnePlus**: App Info → Battery usage → Enable **Allow background activity** and **Allow auto-launch**.
   * **vivo OriginOS**: App Info → Battery usage → Allow **High background power usage**.
   * **Huawei HarmonyOS**: App Launch → Disable automatic management and check all three manual toggles (Auto-launch / Secondary launch / Run in background).
3. **Lock OBS in Recents**
   * Open the Android Recent Apps switcher and lock the OBS Studio card so aggressive memory cleanup doesn't swipe it out during heavy games.
4. **Keep Screen Awake (When OBS Is Visible)**
   * Under **Settings → General → Output**, keep **Keep screen awake during streaming or recording** enabled when using your device on a stand as a switcher or monitor.

> [!WARNING]
> Due to Android privacy enforcement, **Built-in Camera (Camera2)** must yield camera hardware and pause frames when OBS moves to the background, resuming automatically once you bring OBS back to the front. Only **Screen Capture**, **USB/UVC capture cards**, and **Media Sources** continue delivering video while backgrounded.
