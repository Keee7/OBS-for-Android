/******************************************************************************
    Copyright (C) 2026 OBS for Android port contributors

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

#include "PluginManager.hpp"
#include "PluginModuleLoader.hpp"

#include <obs-module.h>
#include <util/base.h>

#include <cstdlib>
#include <string>

/* Android 上改用**扫目录**装载插件，与 Linux/BSD 的 loader 同一套逻辑
 * （obs_load_plugins() → find_modules_in_path() → os_glob()）。
 *
 * 前提是插件 .so 是真实文件系统上的文件：AndroidRuntimeBootstrap 在
 * OBSApp::loadAppModules() 里（**早于** obs_load_core_modules()）把 APK 的
 * assets/obs-plugins/ 解到 $OBS_ROOT_PATH/lib，这里扫这个目录。
 *
 * 分工：
 *   * 随包的核心插件（obs_core_modules[]，由 plugins/CMakeLists.txt 的
 *     set_obs_core_modules() 生成）由 libobs/obs-core-modules-loader-android.c
 *     以 OBS_MODULE_TYPE_CORE 先装载 —— 它们不会出现在插件管理器里；
 *   * core 之外的内置插件（可选采集插件）与用户自己放进 $OBS_ROOT_PATH/lib 的插件
 *     在这里以 OBS_MODULE_TYPE_PLUGIN 载入 → 出现在插件管理器里、可禁用。
 *   * core 重复项会被 obs-module.c:516-526 的 "already loaded" 检查跳过。
 *
 * 用户放插件：往 $OBS_ROOT_PATH/lib 丢一个 .so 就能装、删掉就卸载，不需要重新打包。
 * （$OBS_ROOT_PATH = 应用私有 files/obsroot，adb/文件管理器可写。） */

namespace {

using State = OBS::PluginManager::State;

} // namespace

namespace OBS {

State PluginManager::loadPlugins(bool /*usePortableMode*/)
{
	const char *rootEnv = std::getenv("OBS_ROOT_PATH");
	if (!rootEnv || !*rootEnv) {
		blog(LOG_ERROR, "[android] OBS_ROOT_PATH 未设置，无法定位插件目录");
		return State::Failure;
	}

	/* 注意：插件 .so 的解压（EnsureAndroidPluginBinaries）**不在这里** ——
	 * obs_load_core_modules() 比这里先跑，它要按 obs_core_modules[] 找 .so，
	 * 所以解压必须在 OBSApp::loadAppModules() 最前面完成。这里只扫目录。 */

	/* bin 不含 %module% ⇒ find_modules_in_path() 按"平铺目录"处理：
	 * glob 出 <root>/lib 下的所有 .so，逐个当模块加载。 */
	const std::string bin = std::string(rootEnv) + "/lib";
	const std::string data = std::string(rootEnv) + "/share/obs/obs-plugins/%module%";

	ModuleLoadInfo info = {.path_info = {.binary = bin.c_str(), .data = data.c_str()},
			       .type = OBS_MODULE_TYPE_PLUGIN,
			       .name = nullptr};

	const int failed = loadPluginsByInfo(info, failedModules_);

	blog(LOG_INFO, "[android] 插件扫描 %s：失败 %d 个", bin.c_str(), failed);
	for (const std::string &m : failedModules_)
		blog(LOG_WARNING, "[android] 插件装载失败 %s", m.c_str());

	return failed > 0 ? State::PartialFailure : State::Success;
}

State PluginManager::loadLegacyPlugins(bool /*usePortableMode*/)
{
	/* Android 没有"旧式插件目录布局"（$HOME/obs-studio/plugins/…），无需兼容。 */
	return State::Success;
}

} // namespace OBS
