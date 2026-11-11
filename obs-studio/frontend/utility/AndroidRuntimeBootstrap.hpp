/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

/* 3-2 ②：APK 里的数据落到应用私有目录，再用 OBS_ROOT_PATH 公布出来。
 *
 * 为什么前端自己也要做一遍这件事：libobs 的 find_libobs_data_file()/
 * add_default_module_paths()（libobs/obs-android.c）和平台的 GetDataFilePath()
 * （utility/platform-android.cpp）读的都是同一个 OBS_ROOT_PATH，谁先要数据谁就要求它
 * 已经就位 —— 而第一次要数据的是 OBSApp 构造之后的 InitLocale()。
 *
 * 只在 Android 上编译（见 cmake/os-android.cmake 的 target_sources）。 */

#pragma once

void PrepareAndroidRuntimePaths();

/* 把 APK 的 assets/obs-plugins/ 解到 $OBS_ROOT_PATH/lib（幂等）。
 *
 * 必须在 QApplication 之后调用 —— Qt 的 `assets:` 引擎是平台插件初始化时注册的，
 * 而 PrepareAndroidRuntimePaths() 跑在 main() 最前面（QApplication 之前），那时
 * `assets:` 还不可用。插件装载发生在 OBSApp（QApplication 子类）构造之后，
 * 所以在 OBSApp::loadAppModules() 最前面调。
 *
 * 必须在 obs_load_core_modules() **之前**完成：core loader 按 obs_core_modules[]
 * 逐个 find_core_module($ROOT/lib/<name>)，文件不在就装不上。
 *
 * 与数据解包不同：$ROOT/lib 是**用户也能写**的目录（放自己的插件、删掉即卸载），
 * 所以这里只删"上一版登记过、这一版没有"的文件，用户自己的文件永远不碰。 */
void EnsureAndroidPluginBinaries();
