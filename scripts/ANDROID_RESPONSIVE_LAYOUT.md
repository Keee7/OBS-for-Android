# Android Responsive Layout Design / Android 响应式布局设计规范

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

OBS for Android implements a dynamic responsive layout engine based on Qt logical pixels, providing an adaptive landscape workflow across mobile phones, foldables, tablets, and multi-window split-screen modes.

### 1. Breakpoint Engine & Hysteresis

* **Logical Pixel Breakpoint**: Layout transitions depend on the active window geometry rather than hardware device identifiers:
  * **Compact Mode**: Engaged when window width `< 952` or height `< 552` logical pixels (optimized for smartphones and small windows).
  * **Spacious Mode**: Engaged when window geometry expands to at least `1000 × 600` logical pixels (optimized for tablets and large displays).
* **48px Hysteresis Protection**: A 48-pixel hysteresis band prevents alternating reflow oscillations near breakpoint boundaries during window resizing or display transitions.

### 2. Mobile Space Optimization

* **Adaptive Touch Row Heights**: List items in Scenes and Sources adopt 48px row heights in spacious mode for comfortable touch interaction, while compact mode uses 32px rows to maximize vertical content density on smaller screens.
* **Contextual Toolbar Collapsing**: When no source is selected, compact mode automatically hides the contextual toolbar, dedicating maximum screen space to the video preview canvas.
* **Clamped Dialog Dimensions**: Max dialog bounds are clamped to the intersection of screen available area and parent window geometry, ensuring settings dialogs never extend off-screen.
* **Debounced Event Coalescing**: Window resize events are debounced with a 120 ms one-shot timer to avoid unnecessary per-frame layout recalculations.

### 3. Policy Verification

Run the standalone layout policy test:

```sh
c++ -std=c++17 -Wall -Wextra -Werror scripts/android-layout-test/policy.cpp -o android-layout-test
./android-layout-test
```

---

<a name="简体中文"></a>
## 简体中文

OBS for Android 实现了基于 Qt 逻辑像素的响应式布局设计，专为横屏交互打造，兼顾手机、折叠屏、平板以及多任务分屏下的操作体验。

### 1. 动态断点与滞回保护

* **逻辑像素断点**：布局策略依据当前应用窗口的可用尺寸动态计算，不与特定硬件型号绑定：
  * **紧凑模式**：当可用宽度 `< 952` 或高度 `< 552` 逻辑像素时自动激活（针对常规手机及小窗口）；
  * **宽松模式**：当窗口恢复至 `≥ 1000 × 600` 逻辑像素时切换回宽松触控模式（针对平板全屏）。
* **48px 滞回缓冲区**：通过 48 像素的双稳态滞回区间，彻底消除窗口缩放或临界尺寸时的反复重排抖动。

### 2. 移动端空间优化策略

* **触控行高自适应**：场景与来源列表在宽松模式下采用 48px 行高便于手指触摸；紧凑模式下压缩为 32px，确保在手机屏幕有限的纵向空间内呈现足够的内容行数。
* **工具栏智能隐藏**：在紧凑模式下，若当前未选中任何来源，自动隐藏上下文工具栏，将宝贵的屏幕空间全额让给视频预览画布。
* **对话框几何约束**：弹窗上限严格取系统可用区与父窗口几何的交集，杜绝对话框超出屏幕边界或被虚拟按键遮挡。
* **120ms 防抖重排**：主窗口 resize 事件通过 120ms 单次计时器合并防抖，仅在布局模式发生实质跨越时才重新应用样式，保障操作流畅。

### 3. 独立策略验证

运行独立布局策略验证套件：

```sh
c++ -std=c++17 -Wall -Wextra -Werror scripts/android-layout-test/policy.cpp -o android-layout-test
./android-layout-test
```
