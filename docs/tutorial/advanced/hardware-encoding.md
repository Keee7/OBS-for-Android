# 硬件编码

OBS for Android 针对移动设备重新实现了 `android-mediacodec` 插件，相比传统软编或 CPU 拷贝路径，在功耗和帧率稳定性上有质的提升。

## 1. 零拷贝 Surface 编码架构

* **GPU 纹理直连**：OBS 主渲染管线（OpenGL ES）合成好的每一帧画面，直接提交给 MediaCodec 绑定的输入 **EGL Surface**，**不经过 CPU 内存回读与二次拷贝**。
* **独立共享上下文与 GPU 同步**：编码 Surface 运行在独立的共享 `EGLContext` 上，并通过双向 GPU Sync Fence 与主渲染循环解耦——即使硬件编码器偶发耗时较长，也不会卡死主界面预览。
* **自适应减载保护**：内置帧提交耗时与积压监控（5 秒无输出或队列超过上限时安全报错退出，避免无声卡死）。

## 2. 高级模式下的码率与质量控制

如果在 **设置 → 输出** 将模式切换为 **高级**，可以展开 `Android MediaCodec` 的详细编码属性：

* **Rate control（码率控制）**：
  * **`CBR`**：固定码率（直播推流首选，码率平稳不易引起网络抖动）。
  * **`VBR`**：动态码率（本地录像适用，复杂画面给高码率、静止画面省空间）。
  * **`CQ (device quality range)`**：恒定质量模式（不再按固定 kbps 限制，改用硬件内部质量区间控制）。
* **Hardware quality（硬件质量预设）**：提供 `Automatic`、`Low`、`Medium`、`High`，插件会将其动态映射到设备组件报告的 complexity / quality 区间（部分设备芯片不支持显式指定档位时，建议保持 `Automatic`）。
* **Keyframe Interval（关键帧间隔）**：默认 `2` 秒（直播平台通用要求）。

## 3. 智能探测与自动回退机制

为了避免设备驱动差异导致直接崩溃，OBS 会在输出前自动探测芯片能力：
1. 若选择的编码器不支持当前所需的 `Full` 色彩范围，会自动切换为 **ByteBuffer YUV（`NV12` / `I420`）** 路径，或回退至软件编码器并弹出提示。
2. 若在 HDR 色彩空间下设备不支持硬件 HEVC Main10，会自动回退至 **Software HEVC (x265)**，并在状态栏提示。

## 4. 用 ADB 验证零拷贝硬编是否生效

连接电脑开启 USB 调试后，在推流或录制过程中运行：

```sh
adb logcat -v threadtime -d -s 'OBS-MediaCodec:*'
```

只要日志包含 `event=started hardware=true software=false input=Surface` 与 `event=first-packet headers=valid keyframe=1`，且 `event=progress` 帧数与字节数持续递增，就说明 GPU 零拷贝硬件编码已完美工作。
