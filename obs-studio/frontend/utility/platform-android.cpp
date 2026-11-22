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

#include "platform.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSAndroidPath.hpp>

#include <obs-config.h>

#include <util/base.h>
#include <util/platform.h>

#include <QFileInfo>
#include <QLocale>
#include <QWidget>
#include <QStandardPaths>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <vector>

using namespace std;

/* 资源根目录的约定与 libobs 侧完全一致（见 libobs/obs-android.c 顶部注释）：
 * Java 层把 APK 里的资源解包到应用私有目录，再用 OBS_ROOT_PATH 环境变量公布出来，
 * 于是 frontend 的数据就落在 $OBS_ROOT_PATH/share/obs/obs-studio/ 之下 ——
 * 与 Linux 上 <prefix>/share/obs/obs-studio/ 同构。 */
#define ROOT_PATH_ENV "OBS_ROOT_PATH"
#define INSTALL_DATA_PATH OBS_INSTALL_DATA_PATH "/obs-studio/"

static inline bool check_path(const char *data, const char *path, string &output)
{
	ostringstream str;
	str << path << data;
	output = str.str();

	blog(LOG_DEBUG, "Attempted path: %s", output.c_str());

	return os_file_exists(output.c_str());
}

bool GetDataFilePath(const char *data, string &output)
{
	const char *root = getenv(ROOT_PATH_ENV);
	if (root && root[0]) {
		const string root_data = string(root) + "/" + OBS_DATA_PATH + "/obs-studio/";
		if (check_path(data, root_data.c_str(), output)) {
			return true;
		}
	}

	if (const char *env = getenv("OBS_DATA_PATH")) {
		if (env[0] && check_path(data, (string(env) + "/obs-studio/").c_str(), output)) {
			return true;
		}
	}

	if (check_path(data, OBS_DATA_PATH "/obs-studio/", output)) {
		return true;
	}

	if (check_path(data, INSTALL_DATA_PATH, output)) {
		return true;
	}

	return false;
}

string GetDefaultVideoSavePath()
{
	/* 默认放到 MediaStore 受管的公共 Movies/OBS 目录，确保设置页可见且录制可写。 */
	QString path = obsAndroidStorageDefaultVideoDirectory();
	if (path.isEmpty()) {
		path = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
		if (!QFileInfo(path).isDir())
			path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	}

	blog(LOG_INFO, "Default video save path: %s", path.toStdString().c_str());

	return path.toStdString();
}

vector<string> GetPreferredLocales()
{
	const QStringList ui_languages = QLocale::system().uiLanguages();
	const auto obs_locales = GetLocaleNames();

	auto find_obs_locale = [&obs_locales](const QString &candidate) -> string {
		for (const auto &locale_pair : obs_locales) {
			if (QString::fromStdString(locale_pair.first).compare(candidate, Qt::CaseInsensitive) == 0) {
				return locale_pair.first;
			}
		}
		return {};
	};

	auto ui_to_obs = [&obs_locales, &find_obs_locale](const QString &ui) -> string {
		QString normalized = ui;
		normalized.replace('_', '-');
		const QStringList parts = normalized.split('-', Qt::SkipEmptyParts);
		if (parts.isEmpty()) {
			return {};
		}

		QString lang = parts[0].toLower();
		if (lang == "in")
			lang = "id";
		else if (lang == "iw")
			lang = "he";
		else if (lang == "ji")
			lang = "yi";
		else if (lang == "no")
			lang = "nb";

		QString script;
		QString region;
		for (int i = 1; i < parts.size(); ++i) {
			const QString &p = parts[i];
			if (script.isEmpty() && p.size() == 4 && p[0].isLetter()) {
				script = p[0].toUpper() + p.mid(1).toLower();
			} else if (region.isEmpty() &&
				   ((p.size() == 2 && p[0].isLetter() && p[1].isLetter()) ||
				    (p.size() == 3 && p[0].isDigit()))) {
				region = p.toUpper();
			}
		}

		if (lang == "zh") {
			if (script == "Hant") {
				if (const string m = find_obs_locale("zh-TW"); !m.empty())
					return m;
			} else if (script == "Hans") {
				if (const string m = find_obs_locale("zh-CN"); !m.empty())
					return m;
			} else if (region == "TW" || region == "HK" || region == "MO") {
				if (const string m = find_obs_locale("zh-TW"); !m.empty())
					return m;
			}
			if (const string m = find_obs_locale("zh-CN"); !m.empty())
				return m;
		} else if (lang == "sr") {
			if (script == "Latn") {
				if (const string m = find_obs_locale("sr-CS"); !m.empty())
					return m;
			} else if (script == "Cyrl") {
				if (const string m = find_obs_locale("sr-SP"); !m.empty())
					return m;
			} else if (region == "CS" || region == "ME") {
				if (const string m = find_obs_locale("sr-CS"); !m.empty())
					return m;
			}
			if (const string m = find_obs_locale("sr-SP"); !m.empty())
				return m;
		} else if (lang == "en") {
			if (!region.isEmpty()) {
				if (const string m = find_obs_locale("en-" + region); !m.empty())
					return m;
			}
			if (const string m = find_obs_locale("en-US"); !m.empty())
				return m;
		} else if (lang == "pt") {
			if (region == "PT") {
				if (const string m = find_obs_locale("pt-PT"); !m.empty())
					return m;
			}
			if (const string m = find_obs_locale("pt-BR"); !m.empty())
				return m;
		}

		if (!region.isEmpty()) {
			if (const string m = find_obs_locale(lang + "-" + region); !m.empty())
				return m;
		}

		if (const string m = find_obs_locale(lang); !m.empty())
			return m;

		for (const auto &locale_pair : obs_locales) {
			const QString loc = QString::fromStdString(locale_pair.first);
			const int dash = loc.indexOf('-');
			const QString loc_lang = (dash > 0 ? loc.left(dash) : loc).toLower();
			if (loc_lang == lang) {
				return locale_pair.first;
			}
		}

		return {};
	};

	vector<string> result;
	result.reserve(ui_languages.size());

	for (const QString &ui_language : ui_languages) {
		string match = ui_to_obs(ui_language);
		if (!match.size()) {
			continue;
		}

		if (find(begin(result), end(result), match) != end(result)) {
			continue;
		}

		result.emplace_back(match);
	}

	return result;
}

/* Android 没有"窗口永远压在其他应用之上"这种窗口层概念：要跨应用置顶得换
 * TYPE_APPLICATION_OVERLAY 窗口 + SYSTEM_ALERT_WINDOW 权限，Qt 给不了。
 * 这里只把意图记在窗口属性上，让"总是置顶"菜单的勾选状态保持自洽。
 *
 * 不能照抄 platform-x11.cpp 那版 setWindowFlags() + show()：改窗口标志会触发
 * Qt 重建窗口，ANativeWindow 随之销毁，挂在它上面的 gs_swapchain 就没了（S1 实测：
 * surfaceDestroyed 是唯一可靠的销毁信号）。 */
static const char kAlwaysOnTopKey[] = "_obsAndroidAlwaysOnTop";

bool IsAlwaysOnTop(QWidget *window)
{
	return window->property(kAlwaysOnTopKey).toBool();
}

void SetAlwaysOnTop(QWidget *window, bool enable)
{
	window->setProperty(kAlwaysOnTopKey, enable);
}

bool SetDisplayAffinitySupported(void)
{
	return false;
}

/* Qt 6.9.3 的 QStyleHints 只有 colorScheme()，没有高对比度开关（桌面版同样没有，
 * 所以 x11 才去查 XDG portal）。Android 的高对比度文字要读 Settings.Secure，
 * 得等 Java 胶水层（计划 3-5）再实现；返回 false 的效果只是不强制切到
 * com.obsproject.System 高对比主题，功能不受影响。 */
bool HighContrastEnabled()
{
	return false;
}

void TaskbarOverlayInit() {}
void TaskbarOverlaySetStatus(TaskbarOverlayStatus) {}

/* 清单里 QtActivity 已经是 launchMode="singleTop"，系统保证不会起第二个实例；
 * 而且 Android 上没有跨进程命名互斥量的对等物（/tmp 不可用，抽象窗口层也没有）。 */
void CheckIfAlreadyRunning(bool &already_running)
{
	already_running = false;
}
