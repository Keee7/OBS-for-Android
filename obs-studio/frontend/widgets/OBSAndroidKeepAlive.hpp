/* SPDX-License-Identifier: GPL-2.0-or-later */
// 3-5（plan.md §五）：把"OBS 有没有输出在跑"翻译成 Android 前台服务的起停。
// 与 OBSAndroidDisplay 同一类宿主桥，方向相反：那个是把 Surface 递给 obs_display，
// 这个是把 Qt 侧的 Streaming/Recording 信号递给 Java 的 Service。
#pragma once

#include <QString>

// 同步当前 OBS 语言到 Java 前台保活服务，使通知栏文案与 OBS 界面语言保持一致。
void obsAndroidKeepAliveSetLocale(const char *locale);

// running=true → 起前台服务（state 进通知栏文案，如 "streaming"/"recording"/"streaming+recording"）；
// running=false → 停。幂等：重复 true 只刷文案，重复 false 不再惊动 Java。
// 返回是否让 Java 侧认了这次请求；detail 带一行原文（含 Java describe() 的计数器）供 blog。
bool obsAndroidKeepAliveSetRunning(bool running, const char *state, QString *detail = nullptr);

// 一行状态快照（起停请求数 / running / wakelock / 最后一次错误），给日志与冒烟用。
QString obsAndroidKeepAliveStatus();

// 根据设置与当前推流/录制/回放缓存/屏幕采集状态控制 Activity 的 FLAG_KEEP_SCREEN_ON。
void obsAndroidKeepAliveSetScreenOn(bool enabled, bool outputActive);
