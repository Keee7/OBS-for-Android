# IME and UVC Event Loop Architecture / 输入法与 UVC 事件循环优化

[English](#english) | [简体中文](#简体中文)

---

<a name="english"></a>
## English

This document describes the technical implementation of Android soft-keyboard event handling, cursor popup lifecycle management, and UVC event-loop optimization.

### 1. IME Event De-duplication and Cursor Cleanup

* **Duplicate Show Request Filtering**:
  * On certain Android IMEs, tapping backspace can trigger redundant `SHOW_SOFT_INPUT_BY_INSETS_API` requests through `QtInputDelegate`.
  * The patched `QtInputDelegate` suppresses redundant show calls when Android already reports the IME visible, the view retains focus, and the input hint matches, while preserving all key events and cursor updates.
* **Cursor and Selection Popup Cleanup**:
  * Stranded cursor handles occurred when dialogs were dismissed while holding focus.
  * Explicitly tracks view ownership of selection popups and triggers cleanup on view detachment or dialog closure, eliminating ghost handles.

### 2. UVC Event Loop Optimization

* **Atomic Bitmask Handshake**:
  * Replaced the previous 32-slot iteration and per-slot mutex locks with a consolidated atomic bitmask exchange per event loop cycle.
  * Thread-safe error reporting and retry signaling without lock contention on normal ingestion traffic.

---

<a name="简体中文"></a>
## 简体中文

本文档阐述 Android 软键盘事件去重、光标选择手柄生命周期管理以及 UVC 事件循环性能优化的技术实现。

### 1. 输入法事件去重与光标手柄清理

* **过滤多余的键盘显示请求**：
  * 在部分输入法环境下，软键盘连续退格会触发高频的 `SHOW_SOFT_INPUT_BY_INSETS_API` 显示请求；
  * 补丁修改了 `QtInputDelegate`，在系统已确认键盘处于可见状态、编辑框焦点未发生实质迁移时，跳过多余的底层弹起请求，保留正常的按键事件与文字更新。
* **光标与选择手柄清理**：
  * 对话框关闭或焦点切换时，明确按实际拥有手柄的编辑视图执行清理，消除失焦后的光标残影。

### 2. UVC 事件循环优化

* **原子状态位图交接**：
  * 将过去每轮事件循环逐个扫描 32 个传输槽位的互斥锁操作，优化为单次原子重试位图交接；
  * 错误回调在独立状态保护下发布状态，`libusb_clear_halt` 保持在锁外执行，有效消除了正常视频流接收时的互斥锁竞争开销。
