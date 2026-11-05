# 屏幕源透明合成测试

适用版本：`0.1.11-capture-alpha-test` / versionCode `12`。这是采集端透明处理测试版，尚未连接平板验证 VTube Studio → Android 投影 → OBS 的实际 Alpha。

## 使用方法

1. 在 OBS 场景底层添加颜色源或图片，屏幕源放在上层。先用鲜明背景色，便于判断模型周围和半透明边缘。
2. 打开“屏幕采集（MediaProjection）”属性，保持 GPU 屏幕采集开启，将“透明度处理”设为“保留 Alpha（预乘输入）”。如果输入采用直 Alpha，则选择“保留 Alpha（直通）”。默认“兼容”保留旧版绘制方式。
3. 系统授权时选择“单个应用”，选择 VTube Studio。在 VTube Studio 中启用透明背景并隐藏不需要的界面。
4. 确认模型周围显示 OBS 底层背景；更换 OBS 背景色，模型本身不应变透明。检查头发、阴影、半透明饰件等边缘，避免发黑或颜色变暗。若边缘异常，切换直通/预乘模式复测。
5. 调整被采集窗口大小、切换横竖屏。Android 14+ 内容尺寸回调会让 OBS 在下一次尺寸检查时重接 Surface，沿用同一 VirtualDisplay 与授权；尺寸同步周期约一秒。1080p 上限与原生分辨率选项继续生效。源在场景中的缩放/位置仍由原有场景变换决定。
6. 录制一小段，确认录制与预览的合成效果一致。普通 H.264/HEVC 推流发送最终场景画面；接收端无需透明视频支持即可看到人物叠加后的背景。

## 如何判断是否收到了透明帧

透明度选项只保留和正确混合采集帧中已有的 Alpha，不从背景颜色推断透明度。如果两种模式下背景都不透明，不能通过反复切换模式还原已被系统合成的背景。

需要诊断时，可以手动取消 GPU 采集，检查 OBS 日志中的 `android-screen: ... alpha=[最小值,最大值]`。这是 CPU 输入采样值，不代表 GPU 一定相同。全为 `[255,255]` 只说明该帧的采样点不透明；存在小于 255 的值说明输入采样点带有透明度。采样不是全图扫描，不能据此证明所有像素的透明度。检查后手动恢复 GPU。

GPU 模式不为诊断逐帧读回像素，也不自动切换 CPU。CPU 的预乘输入模式需要额外像素转换；高分辨率/高帧率优先使用 GPU。

## 已执行的主机检查

- `scripts/test-android-screen.ps1`：投影显示复用、暂停/恢复、内容尺寸独立于输出 Surface 尺寸、内容旋转、释放后的迟到尺寸事件、新授权不沿用旧尺寸，以及 CPU 预乘反转换。
- `scripts/test-android-interop-shaders.py`：实际 ANGLE GLES 离屏绘制，既有 OES 颜色/Alpha 保留，以及直 Alpha/预乘 Alpha 在透明底、半透明彩色底、不透明彩色底上的合成，覆盖输入 Alpha 0、128、255，检查 RGB 与输出 Alpha。合成测试使用与 OBS 默认 effect 对应的 GLSL 运算，并非在主机运行完整 OBS effect 编译器。

这些检查不能代替 Android 投影服务、玄戒驱动和 VTube Studio 的端到端实机测试。当前未更换应用输出协议，也未加入 Spout2、NDI 或针对 VTube Studio 的私有接口。

Android 单应用共享与内容尺寸回调依据：[Android MediaProjection 官方说明](https://developer.android.com/media/grow/media-projection)。该接口未承诺向普通采集应用交付另一应用渲染前的原始 Alpha。
