# Performance & Power Optimization

Sustained mobile broadcasting pushes SoC thermals and battery hard. Combining OBS for Android's built-in optimizations with smart scene choices prevents thermal throttling and FPS drops.

## 1. Built-In Engine Optimizations

* **Partial Dirty-Region Compositing**: Android RHI `BackingStore` coalesces dirty regions for partial uploads alongside snapshot-optimized `VolumeMeter`, `MediaControls`, and `AbsoluteSlider` widgets, cutting idle and active-mixer CPU usage.
* **End-to-End GPU Zero-Copy**: Screen Capture (`SurfaceTexture`), Android Browser (`AHardwareBuffer`), MediaCodec Surface decode, and MediaCodec EGL Surface encode stay on GPU memory throughout.

## 2. High-Impact Tuning Checklist

| Optimization | What to Do | Impact |
| --- | --- | --- |
| **1. Verify Hardware Encoding** | Use `Android MediaCodec H.264 / HEVC` with `Limited` Color Range | **Largest**: prevents CPU saturation and thermal throttling within minutes |
| **2. Cap Screen Capture at 1080p** | Leave Screen Capture on **1080p cap** instead of native 1.5K/2K | **High**: cuts GPU memory bandwidth and fill rate by ~50–70% |
| **3. Match Resolution & FPS to Thermal Headroom** | Drop outdoor/talk streams to `1080p30` or `720p60` | **High**: halves encoding workload and reduces uplink requirements |
| **4. Limit Browser Overlay FPS** | Set chat overlays' **Max FPS** to `30` (or `15–20`) | **Medium**: avoids unnecessary WebView virtual display redraws |
| **5. Hide Unused Capture Sources** | Click the eye icon on inactive cameras or clips | **Medium**: hidden capture sources stop holding foreground compositor resources |
| **6. Power & Cooling** | Use a USB-C PD hub plus a magnetic/Peltier phone cooler for marathon sessions | **Critical**: prevents charging heat from triggering 30 Hz thermal locks |
