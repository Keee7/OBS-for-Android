# Screen Capture Lifecycle and Geometry Alignment / 屏幕采集生命周期与窗口几何对齐规范

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This document describes session lifecycle management for Android MediaProjection screen capture, as well as window coordinate geometry alignment across multi-window and floating window modes.

### 1. MediaProjection Session Lifecycle

* **Persistent VirtualDisplay Instance**: Complies with Android specifications by retaining a single `VirtualDisplay` instance per user authorization. When the screen rotates or the source is paused, the display surface is dynamically resized and rebound rather than destroyed, eliminating repeated permission prompts.
* **Graceful Termination**: Captures system-level stop notifications (e.g., from the status bar) and safely releases resources, preventing continuous retry loops when authorization ends.

### 2. Multi-Window and Window Geometry Alignment

In split-screen and floating window modes, converting between absolute screen coordinates and parent layouts can lead to rendering offsets.

* **Unified Window Coordinate Space**: Eliminates model-specific heuristics by standardizing on `getLocationInWindow` as the canonical reference coordinate system, aligning Qt widgets with Android parent containers.
* **Dynamic Adaptation**: During window dragging, freeform resizing, and fullscreen transitions, the video preview and toolbar controls maintain accurate geometry and boundary limits without edge clipping or occlusion.

---

<a name="简体中文"></a>
## 简体中文

本文档阐述 Android MediaProjection 屏幕采集的会话生命周期管理，以及多窗口与悬浮小窗模式下的坐标同步实现。

### 1. MediaProjection 会话生命周期

* **单例 VirtualDisplay 维护**：
  依据 Android 规范，单次系统授权仅对应创建单个 VirtualDisplay 实例。在来源停用或屏幕发生旋转时，系统通过 `resize` 与重新挂载 Surface 维持会话，不反复销毁虚拟显示屏，杜绝重复授权弹窗。
* **异常阻断与保护**：
  当从系统通知栏主动停止屏幕共享时，OBS 会捕获授权终止信号并安全解绑资源，避免无效循环拉起。

### 2. 多窗口与小窗坐标几何对齐

在 Android 系统的分屏与悬浮小窗模式下，传统的屏幕绝对坐标与父控件坐标换算容易产生偏移。

* **窗口坐标系归一**：
  统一采用 `getLocationInWindow` 作为基准窗口坐标系计算，将 Qt 界面位置与 Android 原生父容器精准对齐；
* **全生命周期自适应**：
  在小窗拖拽、自由缩放以及全屏切换过程中，主预览区域与控制工具栏均能实时保持正确的几何比例与边界限制，消除了边缘伪影与画面遮挡。
