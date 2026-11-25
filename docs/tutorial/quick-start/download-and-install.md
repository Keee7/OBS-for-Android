# 下载安装

OBS for Android 目前主要通过 GitHub Releases 分发 APK，也支持自行从源码编译。

## 方式一：直接安装 APK（推荐）

1. 打开项目的 [GitHub Releases](https://github.com/Keee7/OBS-for-Android/releases) 页面，选择最新版本的 `arm64-v8a` 安装包（`.apk`）。
2. 在手机或平板的文件管理/浏览器中点击 APK 文件。
3. 根据系统提示授予当前应用「安装未知来源应用」权限后完成安装。

## 方式二：通过电脑 ADB 安装

如果设备已开启「开发者选项 → USB 调试」并连接电脑，可以直接通过 ADB 安装或覆盖升级（保留原有场景和配置）：

```sh
adb install -r path/to/OBS-Android-*.apk
```

## 应用内检查更新

应用默认在启动时自动检查 GitHub Releases 是否有新版本：
* **开关自动检查**：进入 **设置 → 常规 → 更新 → 启动时自动检查更新**。
* **手动检查更新**：在主界面控制区点击 `☰` 按钮 → **帮助(H) → 检查更新**，有新版时会提示并跳转至发布页。

## 自行编译（开发者）

如果你需要修改源码或定制插件模块，请参考仓库的 [BUILDING.md](https://github.com/Keee7/OBS-for-Android/blob/main/BUILDING.md)，支持 GitHub Actions 自动构建、Ubuntu/WSL2 以及 Windows 原生构建。
