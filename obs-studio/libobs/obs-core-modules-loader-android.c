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

#include "obs-core-modules-loader.h"

#include <obs-internal.h>
#include <obs.h>
#include <obs-core-modules.h>

#include <android/log.h>
#include <stdlib.h>

/* 这条路径是启动关键路径：装载失败会让随包的核心插件变成"普通插件"（出现在插件
 * 管理器里）或者干脆缺功能。而 blog() 在 Android 上只写应用私有目录里的日志文件，
 * 非 debuggable 包 run-as 不进去 —— 所以这里额外打一份到 logcat。 */
#define CORE_LOG(...) __android_log_print(ANDROID_LOG_INFO, "OBS-core", __VA_ARGS__)

extern bool find_core_module(struct obs_runtime_module_info *info, obs_find_module_callback2_t callback,
			     void *data);

void load_core_modules(obs_find_module_callback2_t callback, void *data)
{
	const char *root = getenv("OBS_ROOT_PATH");
	const char *bundle = getenv("OBS_ANDROID_BUNDLED_PLUGIN_DIR");
	if (!root || !*root || !bundle || !*bundle) {
		blog(LOG_ERROR, "[android] Android plugin bundle not initialized");
		CORE_LOG("Android plugin bundle not initialized");
		return;
	}

	struct dstr bin = {0};
	struct dstr dat = {0};
	int loaded = 0;

	dstr_copy(&dat, root);
	dstr_cat(&dat, "/share/obs/obs-plugins/%module%");

	for (unsigned int i = 0; i < obs_core_modules_count; i++) {
		const char *name = obs_core_modules[i];

		dstr_copy(&bin, bundle);
		dstr_cat(&bin, "/");
		dstr_cat(&bin, name);

		struct obs_runtime_module_info info = {
			.path_info = {.binary = bin.array, .data = dat.array},
			.type = OBS_MODULE_TYPE_CORE,
			.name = name,
		};

		if (find_core_module(&info, callback, data)) {
			++loaded;
		} else {
			/* 不 break、也不计失败：缺一个就降级 —— 后面 loadPlugins() 扫同一个
			 * 目录时会把它当普通插件再试一次（可用的功能不会丢，只是会出现在
			 * 插件管理器里）。这样也避免 obs_load_core_modules() 返回 false
			 * 导致 loadAllPlugins 直接抛"Failed to load core OBS modules"。 */
			blog(LOG_WARNING, "[android] 核心模块 %s 未找到：%s.so（稍后按普通插件重试）", name,
			     bin.array);
			CORE_LOG("核心模块未找到：%s（%s.so）", name, bin.array);
		}
	}

	blog(LOG_INFO, "[android] 核心模块装载：%d/%u", loaded, obs_core_modules_count);
	CORE_LOG("核心模块装载：%d/%u", loaded, obs_core_modules_count);
	if (loaded == 0) {
		blog(LOG_ERROR,
		     "[android] 一个核心模块都没找到 —— 插件是否已解到 %s/lib？（检查 EnsureAndroidPluginBinaries 的调用时机）",
		     root);
		CORE_LOG("致命：一个核心模块都没找到，root=%s", root);
	}

	dstr_free(&bin);
	dstr_free(&dat);
}
