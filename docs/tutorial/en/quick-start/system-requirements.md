# System & Device Requirements

Before getting started, verify that your phone or tablet meets the following hardware, OS, and permission requirements.

## 1. Hardware & OS Baseline

* **CPU Architecture**: **`arm64-v8a` only** (supported on virtually all modern 64-bit Android devices; 32-bit devices and x86 emulators are not supported).
* **OS Version**: Minimum **Android 10 (API 29)**, Target SDK 35.
  * **Android 13+**: Unlocks hardware **HEVC Main10 (10-bit HDR)** encoding (when supported by the SoC).
  * **Android 14+**: Native **single-app screen sharing** with automatic capture resize when the target window changes size or orientation.
* **WebView Requirement**: Using **Android Browser** for chat overlays or HTML widgets requires **Android System WebView** to be enabled on the device (preinstalled on nearly all devices).

## 2. Required Permissions

OBS requests permissions on first launch or when you activate a corresponding source:

| Permission | When Requested | Purpose |
| --- | --- | --- |
| **Microphone (`RECORD_AUDIO`)** | First launch | Captures built-in mic, wired headset, USB audio interface, or capture-card audio via AAudio |
| **Notifications (`POST_NOTIFICATIONS`)** | First launch | Keeps the foreground service notification visible during streaming, recording, or capture so Android does not kill the background process |
| **Camera (`CAMERA`)** | First launch | Captures front or rear lenses using **Built-in Camera (Camera2)** |
| **Screen Sharing (MediaProjection)** | Adding/enabling **Screen Capture** | Captures the whole display or a single app along with internal system audio |
| **USB Device Permission** | Connecting a UVC card or camera | Grants user-space USB access to UVC capture cards, USB webcams, or Nikon Z cameras |
| **Directory Access (SAF)** | Tapping **Browse** in Recording Settings | Grants persistent write permission to your chosen shared folder (such as `Download/obs`) |

> [!TIP]
> If you accidentally denied a permission on first launch, open Android **Settings → Apps → OBS Studio → Permissions**, switch the permission to **Allow**, and reopen the source's **Properties** dialog.

## 3. Key Differences from Desktop OBS

1. **Adaptive Landscape Dual-Layout**: Fixed landscape orientation; automatically uses a compact layout on phones/small windows and expands full multi-column docks on tablets.
2. **Menu Bar Inside `☰`**: Desktop's `File / Edit / View / Tools / Help` menus live under the `☰` hamburger button in the Controls dock.
3. **Custom RTMP Stream Setup**: OAuth logins (Twitch/YouTube) are disabled; enter your **Server** URL and **Stream Key** manually under **Custom...**.
4. **Recording Containers Limited to Hybrid MP4 / MOV**: Because Android forbids spawning external `ffmpeg-mux` child processes, local recordings use OBS's in-process hybrid writer.
