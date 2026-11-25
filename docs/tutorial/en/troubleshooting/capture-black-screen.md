# Black Screen Capture

If the preview canvas or a specific source displays only black frames, inspect each source type using the checklist below.

## 1. Screen Capture (MediaProjection) Shows Black

* **Missing or Expired Consent**: Open Screen Capture **Properties**, confirm the status isn't waiting for permission, and tap **Request Screen Share Permission** again.
* **Target App Sets `FLAG_SECURE`**: Banking apps, password prompts, payment sheets, and DRM-protected streaming players enforce `FLAG_SECURE`, causing Android to emit blank frames intentionally.
* **GPU Capture Driver Quirk**: On rare GPU drivers where OES `SurfaceTexture` sampling malfunctions, open Screen Capture **Properties**, uncheck **GPU Screen Capture (Experimental)**, and test CPU capture fallback.

## 2. Built-in Camera (Camera2) Shows Black

* **Missing Camera Permission**: Check **Android Settings → Apps → OBS Studio → Permissions** and ensure Camera access is set to **Allow**.
* **App Was Backgrounded or Camera Busy**: Android revokes camera access in background. OBS resumes automatically on returning to foreground; if another app (stock Camera, video call) is holding the sensor, close it and reopen the source **Properties**.

## 3. UVC Capture Card Shows Black

* **USB Permission Not Granted**: With the card plugged in, open UVC **Properties**, tap **Request USB Permission**, and accept the system dialog.
* **No HDMI Input or YUV Mismatch**: Confirm the console or camera is powered on and outputting video. If the status or log reports an unsupported YUV format, uncheck **Lock selected YUV format** so OBS can negotiate MJPEG or another supported descriptor.

## 4. Android Browser (WebView) Shows Black or Opaque White

* **Still Loading or Lost Surface**: Confirm the top status reads **"Browser is receiving frames"**, or tap **Refresh** at the bottom.
* **Transparent Background Still Covers Scene**: Many web pages declare an opaque `body` background color. Add `body { background-color: transparent !important; }` under **Custom CSS**.
