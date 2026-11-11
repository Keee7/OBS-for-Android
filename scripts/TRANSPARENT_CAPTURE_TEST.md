# Single-App Window Capture and Alpha Compositing / 单应用窗口捕获与 Alpha 透明度测试指南

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This guide describes how to configure and verify single-application window capture with Alpha channel preservation in Android 14+ environments. This feature is intended for transparent avatar applications (VTuber tools), transparent UI overlays, and layered compositing.

### 1. Setup Procedure

1. **Scene Preparation**:
   * Add a distinct background source (such as a solid high-contrast color or image) at the bottom of the scene to easily inspect semi-transparent edges.
   * Add a "Screen Capture (MediaProjection)" source on top.
2. **Transparency Handling Options**:
   * Open the Screen Capture source properties and ensure "GPU Screen Capture" is enabled.
   * Configure "Transparency Handling" based on the target app's output mode:
     * **Preserve Alpha (Premultiplied)**: Selected when the target application renders using premultiplied alpha (standard for most 3D/graphics engines).
     * **Preserve Alpha (Straight)**: Selected when the target application outputs straight alpha.
     * **Compatibility**: Retains the legacy opaque rendering pipeline.
3. **Target Selection**:
   * In the system MediaProjection prompt, select "A single app" and pick the target application.
   * Ensure the target application has enabled its transparent background option.
4. **Visual Verification**:
   * Confirm that the background beneath the subject displays cleanly through transparent regions.
   * Inspect hair strands, drop shadows, and semi-transparent accessories to ensure edges do not darken or exhibit black border artifacts (switch between straight and premultiplied if artifacts appear).
   * Resize the target application window; verify that the captured frame scales and syncs dynamically via Android 14+ size callbacks.

### 2. Technical Details

* **Alpha Preservation**: The pipeline preserves the actual Alpha channel delivered by the Android window buffer; if the underlying OS compositor has already flattened the background into an opaque buffer, transparency cannot be reconstructed retroactively.
* **GPU Path**: GPU transparency compositing executes entirely within the shader pipeline without CPU memory readbacks.

---

<a name="简体中文"></a>
## 简体中文

本指南说明如何在 Android 14+ 环境下测试单应用窗口捕获及其 Alpha 透明通道合成功能。该功能主要用于虚拟主播动捕形象、透明 UI 浮层或特定应用画面的透明背景叠加。

### 1. 使用与配置步骤

1. **准备 OBS 场景**：
   * 在 OBS 场景底部添加一个背景源（如高对比度纯色源或图片），以便直观观察捕获画面的边缘半透明效果；
   * 在上层添加“屏幕采集（MediaProjection）”源。
2. **配置透明度处理模式**：
   * 打开屏幕采集源属性，确保开启“GPU 屏幕采集”；
   * “透明度处理”选项根据待捕获应用输出特性进行选择：
     * **保留 Alpha（预乘输入）**：目标应用使用预乘 Alpha（Premultiplied Alpha）输出时选用（大多数 3D/渲染引擎的默认方式）；
     * **保留 Alpha（直通）**：目标应用使用未预乘（Straight Alpha）输出时选用；
     * **兼容模式**：保持原有的常规不透明绘制方式。
3. **选择捕获目标**：
   * 在系统的 MediaProjection 共享授权弹窗中，选择“单个应用”，并选中需要捕获的目标应用；
   * 确保目标应用本身已启用“透明背景”选项。
4. **效果验证**：
   * 观察目标主体周围是否正确透出 OBS 底层背景；
   * 仔细检查发丝、阴影、半透明饰件等边缘过渡区域，确认无黑边或颜色异常变暗（若有异常，尝试在直通与预乘模式间切换）；
   * 调整目标应用窗口尺寸，验证画面比例是否自适应同步。

### 2. 技术原理与说明

* **Alpha 保留机制**：该功能严格解析并合成 Android 窗口交付缓冲区中的实际 Alpha 通道信息；若系统底层合成器已将应用背景压暗合并为不透明帧，则无法逆向反推透明度。
* **性能优化**：GPU 模式下透明合成直接在着色器中完成，不产生额外的 CPU 内存回读；CPU 模式下仅在预乘输入时进行必要的色彩转换。
