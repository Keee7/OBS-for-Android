# Capture Card Compatibility

OBS for Android's `android-capture` plugin speaks UVC directly in user space over `libusb` (supporting both Bulk and Isochronous transfers), so capture cards still work even on phone kernels without `uvcvideo` compiled in.

## 1. Cable & Power Requirements

* **True USB 3.0 (5 Gbps+) for Uncompressed Video**: Running uncompressed `YUYV`, `NV12`, `RGB24`, or `BGR24` at `1080p60` requires both a USB 3.0 capture card and a real USB 3.0 data cable (typically marked 5 Gbps / 10 Gbps with blue/teal pins).
* **Stock Phone Charging Cables Are Usually USB 2.0**: Standard Type-C fast-charging cables max out at 480 Mbps. Capture cards will still connect over them, but only `MJPEG` or lower frame rates fit—locking `YUYV` triggers a USB bandwidth error.
* **Powered PD Hub**: Most capture cards draw `1.5W–3.5W`. Use a USB-C hub with **PD pass-through charging** during long streams to prevent over-current disconnects or rapid battery drain.

## 2. Recommended Format by Capture Card Tier

| Capture Card Tier | Recommended Setup | Notes |
| --- | --- | --- |
| **USB 3.0 Driverless Cards (MS2130, AVerMedia, Elgato, etc.)** | Select an exact hardware preset, or `MJPEG 1080p60` / `YUYV 1080p60` | For crispest visuals and minimum console lag, pair `YUYV` / `NV12` with **Lock selected YUV format** and **Lowest latency**. |
| **USB 2.0 Entry Cards (MS2109, etc.)** | **`MJPEG`** at `1920×1080 @ 30fps` or `1280×720 @ 60fps` | Keep **Lock selected YUV format** unchecked so negotiation uses MJPEG without exceeding 480 Mbps. |
| **10-bit HDR Capture Cards** | `P010` + `Rec. 2100` in **Advanced → Video** | Requires the card itself to advertise `P010` descriptors over USB. |

## 3. UAC Audio & Kernel Interface Locks

* **Auto-Matching USB Audio**: OBS pairs companion UAC audio devices using USB topology path and `VID:PID`. If multiple USB audio endpoints conflict, pick the card explicitly at the bottom of UVC Properties.
* **Kernel `uvcvideo` Conflict**: On rare ROMs with built-in UVC camera support, if OBS warns that claiming the USB interface failed due to kernel `uvcvideo`, replug the adapter or close any stock external-camera app first.
