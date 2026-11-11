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

/* OBS 33.0 把"核心模块"（随安装包的第一方插件）从第三方插件里拆了出来，并且把枚举
 * 方式按平台拆成 obs-core-modules-loader-{mac,win,nix}.c。Android 不属于其中任何一个
 * （libobs/CMakeLists.txt 的 PLATFORM_ID 里没有 Android），所以必须在这里提供
 * load_core_modules()，否则 obs_load_core_modules() 会 undefined reference。
 *
 * 桌面 nix 版是"逐个 find_core_module($exe/../lib/obs-plugins/<name>)"。Android 的
 * 插件由 AndroidRuntimeBootstrap::EnsureAndroidPluginBinaries() 从 APK 的
 * assets/obs-plugins/ 解到 $OBS_ROOT_PATH/lib，路径换成那里，其余逻辑一致。
 *
 * 为什么必须以 OBS_MODULE_TYPE_CORE 装载（而不是当普通插件）：
 *   1. PluginManager 会跳过 core 模块（PluginManager.cpp:42 `obs_is_core_module()`），
 *      所以内置插件不会出现在"插件管理器"里 —— 与桌面行为一致；
 *   2. core 模块不受用户 disabled 配置影响（obs-module.c:502），不会被误禁用；
 *   3. 之后 loadPlugins() 扫同一个目录时，重复项会被 obs-module.c:516-526 的
 *      "already loaded" 检查跳过 —— 于是 core 之外的内置插件（可选采集插件）与用户
 *      自己放进 $OBS_ROOT_PATH/lib 的插件才以 PLUGIN 类型载入、才会出现在管理器里。
 *
 * 清单 obs_core_modules[] 由 plugins/CMakeLists.txt 的 set_obs_core_modules() 生成，
 * 即白名单里除可选项之外的全部模块；具体数量随白名单变化，不要写死。
 *
 * 注意 `find_core_module()` 的 binary 参数**不带扩展名**，函数内部会自己拼
 * get_module_extension()；data 传带 %module% 的模板，由 make_data_directory() 替换。 */

extern bool find_core_module(struct obs_runtime_module_info *info, obs_find_module_callback2_t callback,
			     void *data);

void load_core_modules(obs_find_module_callback2_t callback, void *data)
{
	const char *root = getenv("OBS_ROOT_PATH");
	if (!root || !*root) {
		blog(LOG_ERROR, "[android] OBS_ROOT_PATH 未设置，无法定位核心模块");
		CORE_LOG("OBS_ROOT_PATH 未设置");
		return;
	}

	struct dstr bin = {0};
	struct dstr dat = {0};
	int loaded = 0;

	dstr_copy(&dat, root);
	dstr_cat(&dat, "/share/obs/obs-plugins/%module%");

	for (unsigned int i = 0; i < obs_core_modules_count; i++) {
		const char *name = obs_core_modules[i];

		dstr_copy(&bin, root);
		dstr_cat(&bin, "/lib/");
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
