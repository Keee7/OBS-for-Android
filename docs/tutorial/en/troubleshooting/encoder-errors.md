# Encoder Errors

Quick reference for the most common encoding, color-range, and recording storage errors.

| Symptom / Error Message | Cause | Fix |
| --- | --- | --- |
| **Failed to start output / unsupported encoder profile** | Advanced mode selected an explicit `Hardware quality` (`High`, etc.) or profile rejected by the SoC driver. | Switch **Hardware quality** back to `Automatic` inside MediaCodec properties and restart output. |
| **Fell back to software encoder for Full color range** | **Color Range** is set to `Full` under **Advanced → Video**, which the device's hardware encoder rejects. | Open **Settings → Advanced → Video** and set **Color Range** back to **`Limited`**. |
| **Fell back to Software HEVC encoder** | `Rec. 2100 PQ/HLG` HDR is enabled, but the SoC (or OS below Android 13) lacks hardware HEVC Main10. | Switch back to `Rec. 709` + `NV12` for zero-copy HW encoding, or lower resolution/FPS if keeping software 10-bit HDR. |
| **`MediaCodec produced no video for 5 seconds` / queue timeout** | Encoder stalled due to odd dimensions, thermal shutdown, or resolution exceeding hardware limits. | Use even dimensions at or below `1920×1080` (or SoC limit), let the device cool, and restart OBS. |
| **Insufficient disk space / file path error** | Persistent SAF permission expired, or free storage dropped below the `50 MiB` safety floor. | Free up storage space, or tap **Browse** under **Settings → Output → Recording** to re-authorize your folder. |
| **Missing `ffmpeg_aac` / audio encoder not found** | Rare corrupted install or failed module load. | Reinstall the official Release APK and verify module loading under `☰ → Help (H) → Log Files`. |
