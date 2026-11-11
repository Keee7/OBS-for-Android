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

namespace {

using State = OBS::PluginManager::State;

} // namespace

namespace OBS {

State PluginManager::loadPlugins(bool /*usePortableMode*/)
{
	const char *root = std::getenv("OBS_ROOT_PATH");
	const char *bundle = std::getenv("OBS_ANDROID_BUNDLED_PLUGIN_DIR");
	if (!root || !*root || !bundle || !*bundle)
		return State::Failure;

	const std::string userPlugins = std::string(root) + "/lib";
	const std::string data = std::string(root) + "/share/obs/obs-plugins/%module%";
	ModuleLoadInfo info = {.path_info = {.binary = bundle, .data = data.c_str()},
			       .type = OBS_MODULE_TYPE_PLUGIN,
			       .name = nullptr};

	// Core plugins are loaded first by libobs. Optional bundled plugins
	// take precedence over user plugins with the same module name.
	int failed = loadPluginsByInfo(info, failedModules_);
	info.path_info.binary = userPlugins.c_str();
	failed += loadPluginsByInfo(info, failedModules_);
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
