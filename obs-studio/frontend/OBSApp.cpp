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

#include "OBSApp.hpp"

#include <components/Multiview.hpp>
#include <plugin-manager/PluginManager.hpp>
#include <utility/CrashHandler.hpp>
#include <utility/OBSEventFilter.hpp>
#include <utility/OBSProxyStyle.hpp>
#ifdef __ANDROID__
#include <utility/android-window-fit.hpp>
#include <utility/AndroidRuntimeBootstrap.hpp> /* EnsureAndroidPluginBinaries()：装载前解插件 .so */
#endif
#if defined(_WIN32) || defined(ENABLE_SPARKLE_UPDATER)
#include <utility/models/branches.hpp>
#endif
#include <widgets/OBSBasic.hpp>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif
#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QDesktopServices>
#if defined(_WIN32) || defined(ENABLE_SPARKLE_UPDATER)
#include <QFile>
#endif

#include <QSessionManager>
#ifndef _WIN32
#include <QSocketNotifier>
#endif

#ifdef __ANDROID__
#include "widgets/OBSAndroidBrowser.hpp"
#include "widgets/OBSAndroidAudio.hpp"
#include "widgets/OBSAndroidDisplay.hpp"
#include "widgets/OBSAndroidKeepAlive.hpp" // 前台保活服务语言同步
#include "widgets/OBSAndroidPath.hpp" // 3-4：openUrl(本地路径) 的 Android 替代
#include <QAbstractButton>
#include <QAbstractScrollArea> // 3-4 触摸：长按坐标要按 viewport 系给（itemAt/exec 都吃这个系）
#include <QAbstractSlider>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QScroller>
#include <QScrollerProperties>
#include <QScreen>
#include <QSlider>
#include <QTextEdit>
#include <QComboBox> // P-8 触摸：组合框弹层探针要认出按下落在的是不是 QComboBox
#include <QContextMenuEvent> // 3-4 触摸：长按合成右键
#include <QGestureEvent> // P-8 探针：认出 TapAndHold 手势
#include <QInputMethod>
#include <QJniObject>
#include <QMenu>
#include <QMouseEvent>
#include <QElapsedTimer>
#include <QTimer>
#include <QWindow>
#include <memory>
#include <android/log.h>
// P-1 装载轨迹：APK 不是 debuggable，blog() 只进日志文件不进 logcat，实测看不看得到全靠这条。
#define P1LOG(...) __android_log_print(ANDROID_LOG_INFO, "P1-LOAD", __VA_ARGS__)
#include <android-plugin-modules.h> // 构建期由 plugins/ 白名单生成
#endif

#include <chrono>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#endif

#include "moc_OBSApp.cpp"

using namespace std;

string currentLogFile;
string lastLogFile;
string lastCrashLogFile;

extern bool portable_mode;
extern bool safe_mode;
extern bool multi;
extern bool disable_3p_plugins;
extern bool opt_disable_updater;
extern bool opt_disable_missing_files_check;
extern string opt_starting_collection;
extern string opt_starting_profile;

// GPU hint exports for AMD/NVIDIA laptops
#ifdef _MSC_VER
extern "C" __declspec(dllexport) DWORD NvOptimusEnablement = 1;
extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

constexpr double kDefaultSnapDistance = 5.0;

namespace {

typedef struct UncleanLaunchAction {
	bool useSafeMode = false;
	bool sendCrashReport = false;
} UncleanLaunchAction;

enum class PluginFailureAction { Continue, OpenPluginManager };

PluginFailureAction handlePluginFailure()
{
	QMessageBox pluginWarning;

	pluginWarning.setIcon(QMessageBox::Warning);

	pluginWarning.setWindowTitle(QTStr("PluginFailure.Dialog.Title"));
	pluginWarning.setText(QTStr("PluginFailure.Labels.Text"));

	QPushButton *continueButton =
		pluginWarning.addButton(QTStr("PluginFailure.Dialog.Continue"), QMessageBox::RejectRole);
	QPushButton *handleButton =
		pluginWarning.addButton(QTStr("PluginFailure.Dialog.Open"), QMessageBox::AcceptRole);

	pluginWarning.setDefaultButton(continueButton);

	pluginWarning.exec();

	bool openPluginManager = pluginWarning.clickedButton() == handleButton;

	if (openPluginManager) {
		return PluginFailureAction::OpenPluginManager;
	} else {
		return PluginFailureAction::Continue;
	}
}

UncleanLaunchAction handleUncleanShutdown(bool enableCrashUpload)
{
	UncleanLaunchAction launchAction;

	blog(LOG_WARNING, "Crash or unclean shutdown detected");

#ifdef __ANDROID__
	/* 永久分支（P-3 定案，不进回退清单）：Android 上进程被系统随时回收是常态，"没留干净退出标记"
	   不能当成崩溃证据 ⇒ 这个模态框会在几乎每次自主启动时挡住主窗口。这里不弹框，直接按"正常启动"
	   返回（launchAction 默认 useSafeMode=false、sendCrashReport=false）。
	   注：早期理由里还有一句"阶段 4 前无插件可禁用、safe mode 无意义"，那条已随白名单推进失效 ——
	   留着这分支靠的是上面那条平台事实，不是阶段进度。 */
	blog(LOG_WARNING, "Android: unclean-shutdown marker is meaningless; launching normally");
	Q_UNUSED(enableCrashUpload);
#else
	QMessageBox crashWarning;

	crashWarning.setIcon(QMessageBox::Warning);
	crashWarning.setOption(QMessageBox::Option::DontUseNativeDialog);
	crashWarning.setWindowTitle(QTStr("CrashHandling.Dialog.Title"));
	crashWarning.setText(QTStr("CrashHandling.Labels.Text"));

	if (enableCrashUpload) {
		crashWarning.setInformativeText(QTStr("CrashHandling.Labels.PrivacyNotice"));

		QCheckBox *sendCrashReportCheckbox = new QCheckBox(QTStr("CrashHandling.Checkbox.SendReport"));
		crashWarning.setCheckBox(sendCrashReportCheckbox);
	}

	QPushButton *launchSafeButton =
		crashWarning.addButton(QTStr("CrashHandling.Buttons.LaunchSafe"), QMessageBox::AcceptRole);
	QPushButton *launchNormalButton =
		crashWarning.addButton(QTStr("CrashHandling.Buttons.LaunchNormal"), QMessageBox::RejectRole);

	crashWarning.setDefaultButton(launchNormalButton);

	crashWarning.exec();

	bool useSafeMode = crashWarning.clickedButton() == launchSafeButton;

	if (useSafeMode) {
		launchAction.useSafeMode = true;

		blog(LOG_INFO, "[Safe Mode] Safe mode launch selected, loading third-party plugins is disabled");
	} else {
		blog(LOG_WARNING, "[Safe Mode] Normal launch selected, loading third-party plugins is enabled");
	}

	bool sendCrashReport = (enableCrashUpload) ? crashWarning.checkBox()->isChecked() : false;

	if (sendCrashReport) {
		launchAction.sendCrashReport = true;

		blog(LOG_INFO, "User selected to send crash report");
	}
#endif

	return launchAction;
}

QAccessibleInterface *alignmentSelectorFactory(const QString &classname, QObject *object)
{
	if (classname == QLatin1String("AlignmentSelector")) {
		if (auto *w = qobject_cast<AlignmentSelector *>(object)) {
			return new AccessibleAlignmentSelector(w);
		}
	}
	return nullptr;
}
} // namespace

QObject *CreateShortcutFilter()
{
	return new OBSEventFilter([](QObject *obj, QEvent *event) {
		auto mouse_event = [](QMouseEvent &event) {
			if (!App()->HotkeysEnabledInFocus() && event.button() != Qt::LeftButton) {
				return true;
			}

			obs_key_combination_t hotkey = {0, OBS_KEY_NONE};
			bool pressed = event.type() == QEvent::MouseButtonPress;

			switch (event.button()) {
			case Qt::NoButton:
			case Qt::LeftButton:
			case Qt::RightButton:
			case Qt::AllButtons:
			case Qt::MouseButtonMask:
				return false;

			case Qt::MiddleButton:
				hotkey.key = OBS_KEY_MOUSE3;
				break;

#define MAP_BUTTON(i, j)                       \
	case Qt::ExtraButton##i:               \
		hotkey.key = OBS_KEY_MOUSE##j; \
		break;
				MAP_BUTTON(1, 4);
				MAP_BUTTON(2, 5);
				MAP_BUTTON(3, 6);
				MAP_BUTTON(4, 7);
				MAP_BUTTON(5, 8);
				MAP_BUTTON(6, 9);
				MAP_BUTTON(7, 10);
				MAP_BUTTON(8, 11);
				MAP_BUTTON(9, 12);
				MAP_BUTTON(10, 13);
				MAP_BUTTON(11, 14);
				MAP_BUTTON(12, 15);
				MAP_BUTTON(13, 16);
				MAP_BUTTON(14, 17);
				MAP_BUTTON(15, 18);
				MAP_BUTTON(16, 19);
				MAP_BUTTON(17, 20);
				MAP_BUTTON(18, 21);
				MAP_BUTTON(19, 22);
				MAP_BUTTON(20, 23);
				MAP_BUTTON(21, 24);
				MAP_BUTTON(22, 25);
				MAP_BUTTON(23, 26);
				MAP_BUTTON(24, 27);
#undef MAP_BUTTON
			}

			hotkey.modifiers = TranslateQtKeyboardEventModifiers(event.modifiers());

			obs_hotkey_inject_event(hotkey, pressed);
			return true;
		};

		auto key_event = [&](QKeyEvent *event) {
			int key = event->key();
			bool enabledInFocus = App()->HotkeysEnabledInFocus();

			if (key != Qt::Key_Enter && key != Qt::Key_Escape && key != Qt::Key_Return && !enabledInFocus)
				return true;

			QDialog *dialog = qobject_cast<QDialog *>(obj);

			obs_key_combination_t hotkey = {0, OBS_KEY_NONE};
			bool pressed = event->type() == QEvent::KeyPress;

			switch (key) {
			case Qt::Key_Shift:
			case Qt::Key_Control:
			case Qt::Key_Alt:
			case Qt::Key_Meta:
				break;

#ifdef __APPLE__
			case Qt::Key_CapsLock:
				// kVK_CapsLock == 57
				hotkey.key = obs_key_from_virtual_key(57);
				pressed = true;
				break;
#endif

			case Qt::Key_Enter:
			case Qt::Key_Escape:
			case Qt::Key_Return:
				if (dialog && pressed)
					return false;
				if (!enabledInFocus)
					return true;
				/* Falls through. */
			default:
				hotkey.key = obs_key_from_virtual_key(event->nativeVirtualKey());
			}

			if (event->isAutoRepeat())
				return true;

			hotkey.modifiers = TranslateQtKeyboardEventModifiers(event->modifiers());

			obs_hotkey_inject_event(hotkey, pressed);
			return true;
		};

		switch (event->type()) {
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonRelease:
			return mouse_event(*static_cast<QMouseEvent *>(event));

		/*case QEvent::MouseButtonDblClick:
		case QEvent::Wheel:*/
		case QEvent::KeyPress:
		case QEvent::KeyRelease:
			return key_event(static_cast<QKeyEvent *>(event));

		default:
			return false;
		}
	});
}

string CurrentDateTimeString()
{
	time_t now = time(0);
	struct tm tstruct;
	char buf[80];
	tstruct = *localtime(&now);
	strftime(buf, sizeof(buf), "%Y-%m-%d, %X", &tstruct);
	return buf;
}

#define DEFAULT_LANG "en-US"

#ifndef _WIN32
std::array<int, 2> OBSApp::sigIntFileDescriptor{0, 0};
std::array<int, 2> OBSApp::sigTermFileDescriptor{0, 0};
std::array<int, 2> OBSApp::sigAbrtFileDescriptor{0, 0};
std::array<int, 2> OBSApp::sigQuitFileDescriptor{0, 0};
#endif

bool OBSApp::InitGlobalConfigDefaults()
{
	config_set_default_uint(appConfig, "General", "MaxLogs", 10);
	config_set_default_int(appConfig, "General", "InfoIncrement", -1);
	config_set_default_string(appConfig, "General", "ProcessPriority", "Normal");
	config_set_default_bool(appConfig, "General", "EnableAutoUpdates", true);

#if _WIN32
	config_set_default_string(appConfig, "Video", "Renderer", "Direct3D 11");
#else
#if defined(__APPLE__) && defined(__aarch64__)
	// TODO: Change this value to "Metal" once the renderer has reached production quality
	config_set_default_string(appConfig, "Video", "Renderer", "OpenGL");
#else
	config_set_default_string(appConfig, "Video", "Renderer", "OpenGL");
#endif
#endif

#ifdef _WIN32
	config_set_default_bool(appConfig, "Audio", "DisableAudioDucking", true);
#endif

#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
	config_set_default_bool(appConfig, "General", "BrowserHWAccel", true);
#endif

#ifdef __APPLE__
	config_set_default_bool(appConfig, "Video", "DisableOSXVSync", true);
	config_set_default_bool(appConfig, "Video", "ResetOSXVSyncOnExit", true);
#endif

	return true;
}

bool OBSApp::InitGlobalLocationDefaults()
{
	char path[512];

	int len = GetAppConfigPath(path, sizeof(path), nullptr);
	if (len <= 0) {
		OBSErrorBox(NULL, "Unable to get global configuration path.");
		return false;
	}

	config_set_default_string(appConfig, "Locations", "Configuration", path);
	config_set_default_string(appConfig, "Locations", "SceneCollections", path);
	config_set_default_string(appConfig, "Locations", "Profiles", path);
	config_set_default_string(appConfig, "Locations", "PluginManagerSettings", path);

	return true;
}

void OBSApp::InitUserConfigDefaults()
{
	config_set_default_bool(userConfig, "General", "ConfirmOnExit", true);

	config_set_default_string(userConfig, "General", "HotkeyFocusType", "NeverDisableHotkeys");

	config_set_default_bool(userConfig, "BasicWindow", "PreviewEnabled", true);
	config_set_default_bool(userConfig, "BasicWindow", "PreviewProgramMode", false);
	config_set_default_bool(userConfig, "BasicWindow", "SceneDuplicationMode", true);
	config_set_default_bool(userConfig, "BasicWindow", "SwapScenesMode", true);
	config_set_default_bool(userConfig, "BasicWindow", "SnappingEnabled", true);
	config_set_default_bool(userConfig, "BasicWindow", "ScreenSnapping", true);
	config_set_default_bool(userConfig, "BasicWindow", "SourceSnapping", true);
	config_set_default_bool(userConfig, "BasicWindow", "CenterSnapping", false);
	config_set_default_double(userConfig, "BasicWindow", "SnapDistance", kDefaultSnapDistance);
	config_set_default_bool(userConfig, "BasicWindow", "SpacingHelpersEnabled", true);
	config_set_default_bool(userConfig, "BasicWindow", "RecordWhenStreaming", false);
	config_set_default_bool(userConfig, "BasicWindow", "KeepRecordingWhenStreamStops", false);
	config_set_default_bool(userConfig, "BasicWindow", "SysTrayEnabled", true);
	config_set_default_bool(userConfig, "BasicWindow", "SysTrayWhenStarted", false);
	config_set_default_bool(userConfig, "BasicWindow", "SaveProjectors", false);
	config_set_default_bool(userConfig, "BasicWindow", "ShowTransitions", true);
	config_set_default_bool(userConfig, "BasicWindow", "ShowListboxToolbars", true);
	config_set_default_bool(userConfig, "BasicWindow", "ShowStatusBar", true);
	config_set_default_bool(userConfig, "BasicWindow", "ShowSourceIcons", true);
	config_set_default_bool(userConfig, "BasicWindow", "ShowContextToolbars", true);
	config_set_default_bool(userConfig, "BasicWindow", "StudioModeLabels", true);
	config_set_default_bool(userConfig, "BasicWindow", "SideDocks", true);

#ifdef __ANDROID__
	config_set_default_bool(userConfig, "BasicWindow", "VerticalVolumeControl", false);
#else
	config_set_default_bool(userConfig, "BasicWindow", "VerticalVolumeControl", true);
#endif

	config_set_default_bool(userConfig, "BasicWindow", "MultiviewMouseSwitch", true);

	config_set_default_bool(userConfig, "BasicWindow", "MultiviewDrawNames", true);

	config_set_default_bool(userConfig, "BasicWindow", "MultiviewDrawAreas", true);

	config_set_default_bool(userConfig, "BasicWindow", "MediaControlsCountdownTimer", true);

	config_set_default_bool(App()->GetUserConfig(), "BasicWindow", "MixerShowInactive", false);
	config_set_default_bool(App()->GetUserConfig(), "BasicWindow", "MixerKeepInactiveLast", false);
	config_set_default_bool(App()->GetUserConfig(), "BasicWindow", "MixerShowHidden", false);
	config_set_default_bool(App()->GetUserConfig(), "BasicWindow", "MixerKeepHiddenLast", false);

	config_set_default_int(userConfig, "Appearance", "FontScale", 10);
	config_set_default_int(userConfig, "Appearance", "Density", 1);
}

static bool do_mkdir(const char *path)
{
	if (os_mkdirs(path) == MKDIR_ERROR) {
		OBSErrorBox(NULL, "Failed to create directory %s", path);
		return false;
	}

	return true;
}

static bool MakeUserDirs()
{
	char path[512];

	if (GetAppConfigPath(path, sizeof(path), "obs-studio/basic") <= 0) {
		return false;
	}
	if (!do_mkdir(path)) {
		return false;
	}

	if (GetAppConfigPath(path, sizeof(path), "obs-studio/logs") <= 0) {
		return false;
	}
	if (!do_mkdir(path)) {
		return false;
	}

	if (GetAppConfigPath(path, sizeof(path), "obs-studio/profiler_data") <= 0) {
		return false;
	}
	if (!do_mkdir(path)) {
		return false;
	}

#ifdef _WIN32
	if (GetAppConfigPath(path, sizeof(path), "obs-studio/crashes") <= 0) {
		return false;
	}
	if (!do_mkdir(path)) {
		return false;
	}
#endif

	if (GetAppConfigPath(path, sizeof(path), "obs-studio/updates") <= 0) {
		return false;
	}
	if (!do_mkdir(path)) {
		return false;
	}

	if (GetAppConfigPath(path, sizeof(path), "obs-studio/plugin_config") <= 0) {
		return false;
	}
	if (!do_mkdir(path)) {
		return false;
	}

	return true;
}

constexpr std::string_view OBSProfileSubDirectory = "obs-studio/basic/profiles";
constexpr std::string_view OBSScenesSubDirectory = "obs-studio/basic/scenes";
constexpr std::string_view OBSPluginManagerSubDirectory = "obs-studio/plugin_manager";

static bool MakeUserProfileDirs()
{
	const std::filesystem::path userProfilePath =
		App()->userProfilesLocation / std::filesystem::u8path(OBSProfileSubDirectory);
	const std::filesystem::path userScenesPath =
		App()->userScenesLocation / std::filesystem::u8path(OBSScenesSubDirectory);
	const std::filesystem::path userPluginManagerPath =
		App()->userPluginManagerSettingsLocation / std::filesystem::u8path(OBSPluginManagerSubDirectory);

	if (!std::filesystem::exists(userProfilePath)) {
		try {
			std::filesystem::create_directories(userProfilePath);
		} catch (const std::filesystem::filesystem_error &error) {
			blog(LOG_ERROR, "Failed to create user profile directory '%s'\n%s",
			     userProfilePath.u8string().c_str(), error.what());
			return false;
		}
	}

	if (!std::filesystem::exists(userScenesPath)) {
		try {
			std::filesystem::create_directories(userScenesPath);
		} catch (const std::filesystem::filesystem_error &error) {
			blog(LOG_ERROR, "Failed to create user scene collection directory '%s'\n%s",
			     userScenesPath.u8string().c_str(), error.what());
			return false;
		}
	}

	if (!std::filesystem::exists(userPluginManagerPath)) {
		try {
			std::filesystem::create_directories(userPluginManagerPath);
		} catch (const std::filesystem::filesystem_error &error) {
			blog(LOG_ERROR, "Failed to create user plugin manager directory '%s'\n%s",
			     userPluginManagerPath.u8string().c_str(), error.what());
			return false;
		}
	}

	return true;
}

bool OBSApp::UpdatePre22MultiviewLayout(const char *layout)
{
	if (!layout) {
		return false;
	}

	if (astrcmpi(layout, "horizontaltop") == 0) {
		config_set_int(userConfig, "BasicWindow", "MultiviewLayout",
			       static_cast<int>(MultiviewLayout::HORIZONTAL_TOP_8_SCENES));
		return true;
	}

	if (astrcmpi(layout, "horizontalbottom") == 0) {
		config_set_int(userConfig, "BasicWindow", "MultiviewLayout",
			       static_cast<int>(MultiviewLayout::HORIZONTAL_BOTTOM_8_SCENES));
		return true;
	}

	if (astrcmpi(layout, "verticalleft") == 0) {
		config_set_int(userConfig, "BasicWindow", "MultiviewLayout",
			       static_cast<int>(MultiviewLayout::VERTICAL_LEFT_8_SCENES));
		return true;
	}

	if (astrcmpi(layout, "verticalright") == 0) {
		config_set_int(userConfig, "BasicWindow", "MultiviewLayout",
			       static_cast<int>(MultiviewLayout::VERTICAL_RIGHT_8_SCENES));
		return true;
	}

	return false;
}

bool OBSApp::InitGlobalConfig()
{
	char path[512];

	int len = GetAppConfigPath(path, sizeof(path), "obs-studio/global.ini");
	if (len <= 0) {
		return false;
	}

	int errorcode = appConfig.Open(path, CONFIG_OPEN_ALWAYS);
	if (errorcode != CONFIG_SUCCESS) {
		OBSErrorBox(NULL, "Failed to open global.ini: %d", errorcode);
		return false;
	}

	uint32_t lastVersion = config_get_int(appConfig, "General", "LastVersion");

	if (lastVersion && lastVersion < MAKE_SEMANTIC_VERSION(31, 0, 0)) {
		bool migratedUserSettings = config_get_bool(appConfig, "General", "Pre31Migrated");

		if (!migratedUserSettings) {
			bool migrated = MigrateGlobalSettings();

			config_set_bool(appConfig, "General", "Pre31Migrated", migrated);
			config_save_safe(appConfig, "tmp", nullptr);
		}
	}

	InitGlobalConfigDefaults();
	InitGlobalLocationDefaults();

	std::filesystem::path defaultUserConfigLocation =
		std::filesystem::u8path(config_get_default_string(appConfig, "Locations", "Configuration"));
	std::filesystem::path defaultUserScenesLocation =
		std::filesystem::u8path(config_get_default_string(appConfig, "Locations", "SceneCollections"));
	std::filesystem::path defaultUserProfilesLocation =
		std::filesystem::u8path(config_get_default_string(appConfig, "Locations", "Profiles"));
	std::filesystem::path defaultPluginManagerLocation =
		std::filesystem::u8path(config_get_default_string(appConfig, "Locations", "PluginManagerSettings"));

	if (IsPortableMode()) {
		userConfigLocation = std::move(defaultUserConfigLocation);
		userScenesLocation = std::move(defaultUserScenesLocation);
		userProfilesLocation = std::move(defaultUserProfilesLocation);
		userPluginManagerSettingsLocation = std::move(defaultPluginManagerLocation);
	} else {
		std::filesystem::path currentUserConfigLocation =
			std::filesystem::u8path(config_get_string(appConfig, "Locations", "Configuration"));
		std::filesystem::path currentUserScenesLocation =
			std::filesystem::u8path(config_get_string(appConfig, "Locations", "SceneCollections"));
		std::filesystem::path currentUserProfilesLocation =
			std::filesystem::u8path(config_get_string(appConfig, "Locations", "Profiles"));
		std::filesystem::path currentUserPluginManagerLocation =
			std::filesystem::u8path(config_get_string(appConfig, "Locations", "PluginManagerSettings"));

		userConfigLocation = (std::filesystem::exists(currentUserConfigLocation))
					     ? std::move(currentUserConfigLocation)
					     : std::move(defaultUserConfigLocation);
		userScenesLocation = (std::filesystem::exists(currentUserScenesLocation))
					     ? std::move(currentUserScenesLocation)
					     : std::move(defaultUserScenesLocation);
		userProfilesLocation = (std::filesystem::exists(currentUserProfilesLocation))
					       ? std::move(currentUserProfilesLocation)
					       : std::move(defaultUserProfilesLocation);
		userPluginManagerSettingsLocation = (std::filesystem::exists(currentUserPluginManagerLocation))
							    ? std::move(currentUserPluginManagerLocation)
							    : std::move(defaultPluginManagerLocation);
	}

	bool userConfigResult = InitUserConfig(userConfigLocation, lastVersion);

	return userConfigResult;
}

bool OBSApp::InitUserConfig(std::filesystem::path &userConfigLocation, uint32_t lastVersion)
{
	const std::string userConfigFile = userConfigLocation.u8string() + "/obs-studio/user.ini";

	int errorCode = userConfig.Open(userConfigFile.c_str(), CONFIG_OPEN_ALWAYS);

	if (errorCode != CONFIG_SUCCESS) {
		OBSErrorBox(nullptr, "Failed to open user.ini: %d", errorCode);
		return false;
	}

	MigrateLegacySettings(lastVersion);
	InitUserConfigDefaults();

	return true;
}

void OBSApp::MigrateLegacySettings(const uint32_t lastVersion)
{
	bool hasUserConfigChanges = false;
	bool hasGlobalConfigChanges = false;

	const uint32_t v19 = MAKE_SEMANTIC_VERSION(19, 0, 0);
	const uint32_t v21 = MAKE_SEMANTIC_VERSION(21, 0, 0);
	const uint32_t v23 = MAKE_SEMANTIC_VERSION(23, 0, 0);
	const uint32_t v24 = MAKE_SEMANTIC_VERSION(24, 0, 0);
	const uint32_t v24_1 = MAKE_SEMANTIC_VERSION(24, 1, 0);

	const map<uint32_t, string> defaultsMap{
		{{v19, "Pre19Defaults"}, {v21, "Pre21Defaults"}, {v23, "Pre23Defaults"}, {v24_1, "Pre24.1Defaults"}}};

	for (auto &[version, configKey] : defaultsMap) {
		if (!config_has_user_value(userConfig, "General", configKey.c_str())) {
			bool useOldDefaults = lastVersion && lastVersion < version;
			config_set_bool(userConfig, "General", configKey.c_str(), useOldDefaults);

			hasUserConfigChanges = true;
		}
	}

	if (config_has_user_value(userConfig, "BasicWindow", "MultiviewLayout")) {
		const char *layout = config_get_string(userConfig, "BasicWindow", "MultiviewLayout");

		bool layoutUpdated = UpdatePre22MultiviewLayout(layout);

		hasUserConfigChanges = hasUserConfigChanges | layoutUpdated;
	}

	if (lastVersion && lastVersion < v24) {
		bool disableHotkeysInFocus = config_get_bool(userConfig, "General", "DisableHotkeysInFocus");

		if (disableHotkeysInFocus) {
			config_set_string(userConfig, "General", "HotkeyFocusType", "DisableHotkeysInFocus");
		}

		hasUserConfigChanges = true;
	}

	if (lastVersion && lastVersion < MAKE_SEMANTIC_VERSION(33, 0, 0)) {
		bool migratedUserSettings = config_has_user_value(appConfig, "General", "Pre33.0Migrated");

		if (!migratedUserSettings) {
			double currentSnapDistance = config_get_double(userConfig, "BasicWindow", "SnapDistance");

			if (currentSnapDistance == 10.0) {
				double newDefaultSnapDistance = kDefaultSnapDistance;
				config_set_double(userConfig, "BasicWindow", "SnapDistance", newDefaultSnapDistance);
			}

			config_set_bool(appConfig, "General", "Pre33.0Migrated", true);

			hasUserConfigChanges = true;
			hasGlobalConfigChanges = true;
		}
	}

#ifdef __ANDROID__
	// Older builds persisted these defaults even when the user never changed them.
	if (!config_get_bool(userConfig, "BasicWindow", "MigratedAndroidLayoutDefaults")) {
		if (!config_get_bool(userConfig, "BasicWindow", "ShowContextToolbars"))
			config_remove_value(userConfig, "BasicWindow", "ShowContextToolbars");
		if (config_get_bool(userConfig, "BasicWindow", "VerticalVolumeControl"))
			config_remove_value(userConfig, "BasicWindow", "VerticalVolumeControl");
		config_set_bool(userConfig, "BasicWindow", "MigratedAndroidLayoutDefaults", true);
		hasUserConfigChanges = true;
	}
#endif

	if (hasUserConfigChanges) {
		userConfig.SaveSafe("tmp");
	}

	if (hasGlobalConfigChanges) {
		appConfig.SaveSafe("tmp");
	}
}

static constexpr string_view OBSGlobalIniPath = "/obs-studio/global.ini";
static constexpr string_view OBSUserIniPath = "/obs-studio/user.ini";

bool OBSApp::MigrateGlobalSettings()
{
	char path[512];

	int len = GetAppConfigPath(path, sizeof(path), nullptr);
	if (len <= 0) {
		OBSErrorBox(nullptr, "Unable to get global configuration path.");
		return false;
	}

	std::string legacyConfigFileString;
	legacyConfigFileString.reserve(strlen(path) + OBSGlobalIniPath.size());
	legacyConfigFileString.append(path).append(OBSGlobalIniPath);

	const std::filesystem::path legacyGlobalConfigFile = std::filesystem::u8path(legacyConfigFileString);

	std::string configFileString;
	configFileString.reserve(strlen(path) + OBSUserIniPath.size());
	configFileString.append(path).append(OBSUserIniPath);

	const std::filesystem::path userConfigFile = std::filesystem::u8path(configFileString);

	if (std::filesystem::exists(userConfigFile)) {
		OBSErrorBox(nullptr,
			    "Unable to migrate global configuration - user configuration file already exists.");
		return false;
	}

	try {
		std::filesystem::copy(legacyGlobalConfigFile, userConfigFile);
	} catch (const std::filesystem::filesystem_error &) {
		OBSErrorBox(nullptr, "Unable to migrate global configuration - copy failed.");
		return false;
	}

	return true;
}

bool OBSApp::InitLocale()
{
	ProfileScope("OBSApp::InitLocale");

	const char *lang = config_get_string(userConfig, "General", "Language");
	bool userLocale = config_has_user_value(userConfig, "General", "Language");
	if (!userLocale || !lang || lang[0] == '\0') {
		lang = DEFAULT_LANG;
	}

	locale = lang;

	// set basic default application locale
	if (!locale.empty()) {
		QLocale::setDefault(QLocale(QString::fromStdString(locale).replace('-', '_')));
	}

	string englishPath;
	if (!GetDataFilePath("locale/" DEFAULT_LANG ".ini", englishPath)) {
		OBSErrorBox(NULL, "Failed to find locale/" DEFAULT_LANG ".ini");
		return false;
	}

	textLookup = text_lookup_create(englishPath.c_str());
	if (!textLookup) {
		OBSErrorBox(NULL, "Failed to create locale from file '%s'", englishPath.c_str());
		return false;
	}

	bool defaultLang = astrcmpi(lang, DEFAULT_LANG) == 0;

	if (userLocale && defaultLang) {
		return true;
	}

	if (!userLocale && defaultLang) {
		for (auto &locale_ : GetPreferredLocales()) {
			if (locale_ == lang) {
				return true;
			}

			stringstream file;
			file << "locale/" << locale_ << ".ini";

			string path;
			if (!GetDataFilePath(file.str().c_str(), path)) {
				continue;
			}

			if (!text_lookup_add(textLookup, path.c_str())) {
				continue;
			}

			blog(LOG_INFO, "Using preferred locale '%s'", locale_.c_str());
			locale = locale_;

			// set application default locale to the new chosen one
			if (!locale.empty()) {
				QLocale::setDefault(QLocale(QString::fromStdString(locale).replace('-', '_')));
			}

			return true;
		}

		return true;
	}

	stringstream file;
	file << "locale/" << lang << ".ini";

	string path;
	if (GetDataFilePath(file.str().c_str(), path)) {
		if (!text_lookup_add(textLookup, path.c_str())) {
			blog(LOG_ERROR, "Failed to add locale file '%s'", path.c_str());
		}
	} else {
		blog(LOG_ERROR, "Could not find locale file '%s'", file.str().c_str());
	}

	return true;
}

#if defined(_WIN32) || defined(ENABLE_SPARKLE_UPDATER)
void ParseBranchesJson(const std::string &jsonString, vector<UpdateBranch> &out, std::string &error)
{
	JsonBranches branches;

	try {
		nlohmann::json json = nlohmann::json::parse(jsonString);
		branches = json.get<JsonBranches>();
	} catch (nlohmann::json::exception &e) {
		error = e.what();
		return;
	}

	for (const JsonBranch &json_branch : branches) {
#ifdef _WIN32
		if (!json_branch.windows) {
			continue;
		}
#elif defined(__APPLE__)
		if (!json_branch.macos) {
			continue;
		}
#endif

		UpdateBranch branch = {
			QString::fromStdString(json_branch.name),
			QString::fromStdString(json_branch.display_name),
			QString::fromStdString(json_branch.description),
			json_branch.enabled,
			json_branch.visible,
		};
		out.push_back(branch);
	}
}

bool LoadBranchesFile(vector<UpdateBranch> &out)
{
	string error;
	string branchesText;

	BPtr<char> branchesFilePath = GetAppConfigPathPtr("obs-studio/updates/branches.json");

	QFile branchesFile(branchesFilePath.Get());
	if (!branchesFile.open(QIODevice::ReadOnly)) {
		error = "Opening file failed.";
		goto fail;
	}

	branchesText = branchesFile.readAll().toStdString();
	if (branchesText.empty()) {
		error = "File empty.";
		goto fail;
	}

	ParseBranchesJson(branchesText, out, error);
	if (error.empty()) {
		return !out.empty();
	}

fail:
	blog(LOG_WARNING, "Loading branches from file failed: %s", error.c_str());
	return false;
}
#endif

void OBSApp::SetBranchData(const string &data)
{
#if defined(_WIN32) || defined(ENABLE_SPARKLE_UPDATER)
	string error;
	vector<UpdateBranch> result;

	ParseBranchesJson(data, result, error);

	if (!error.empty()) {
		blog(LOG_WARNING, "Reading branches JSON response failed: %s", error.c_str());
		return;
	}

	if (!result.empty()) {
		updateBranches = result;
	}

	branches_loaded = true;
#else
	UNUSED_PARAMETER(data);
#endif
}

std::vector<UpdateBranch> OBSApp::GetBranches()
{
	vector<UpdateBranch> out;
	/* Always ensure the default branch exists */
	out.push_back(UpdateBranch{"stable", "", "", true, true});

#if defined(_WIN32) || defined(ENABLE_SPARKLE_UPDATER)
	if (!branches_loaded) {
		vector<UpdateBranch> result;
		if (LoadBranchesFile(result)) {
			updateBranches = result;
		}

		branches_loaded = true;
	}
#endif

	/* Copy additional branches to result (if any) */
	if (!updateBranches.empty()) {
		out.insert(out.end(), updateBranches.begin(), updateBranches.end());
	}

	return out;
}

#ifdef __ANDROID__
/* Android 触摸/按键补齐。两件事性质相同 —— 平台把事件送进来了，但 OBS 那套桌面语义没人接 ——
 * 所以合在一个 app 级事件过滤器里：
 * ① 3-4 长按 = 右键。Android 上 Qt 把触摸转成鼠标事件（实测长按 1.2 s 只把列表项选中了，
 * 不出上下文菜单 —— TapAndHold 手势在 QWidget 这条路上没人接），而 OBS 的场景/源/混音器菜单
 * 全挂在 customContextMenuRequested 上，没有右键就永远点不出来。
 * 不拦原始事件：按下照常发给控件（选中、滚动行为都不变），只是同时起一个计时器；到点还没松手
 * 也没滑出阈值，就照 Qt 处理 Key_Menu 的做法，给"最近的声明了上下文菜单策略的祖先控件"送一条
 * QContextMenuEvent(Mouse)。触发后的那次松手**只在真的弹起了菜单时**吞掉（免得给列表补一次"点击"）；
 * 什么都没弹就照常交回控件 —— 否则长按等于把按钮的 clicked 吃掉，见 fire() 的返回值注释。
 * ② 返回键 = Escape，见下面 KeyPress 那一格（build 74）。
 * 不用 Q_OBJECT：只要 eventFilter 虚函数 + 成员 QTimer，省掉一次 moc。 */
/* 把顶层窗口压进屏幕可用区（全局兜底，B 方案）。
 *
 * 背景：OBS 的对话框尺寸是按桌面设计的 —— 添加源 1040x620、设置 981x730、
 * 高级音频 1100x340、滤镜 861x726 …，而这块手机横屏的逻辑尺寸只有 873x393，
 * 于是底部按钮和右侧内容直接落在屏外、点不到。之前零散地在几个对话框的 showEvent
 * 里"收高度"处理过（OBSBasicSettings 是上游自带，OBSBasicFilters 是本移植早期加的），
 * 但都只收高度、且没覆盖最常用的"添加源"。
 *
 * 这里改成全局兜底：任何顶层窗口显示后，只要比可用区大就压到可用区并居中。
 * 覆盖全部对话框，以后新增的也自动受益。
 *
 * 两个注意点：
 * 1) 用 singleShot(0) 延迟一拍，不在 Show 里同步 resize —— Show 那一刻布局还没跑完，
 *    此时 size() 还是 .ui 里的设计尺寸，改完会被随后的布局覆盖。
 * 2) resize() 受控件最小尺寸约束（minimumSize / 布局的 minimumSizeHint）。
 *    压不下去时"实际"会大于"目标"——日志里两个数都打出来，一眼能看出是不是被挡住了。
 *    那种情况需要单独处理（内部套 QScrollArea，或放宽布局约束）。 */
static void clampTopLevelToScreen(QWidget *w)
{
	const AndroidFitResult r = androidFitTopLevelToScreen(w);
	if (!r.resized)
		return; /* 尺寸已经合适（或本来就不该压），不产生任何可见动作 */

	/* resize 被挡住，说明这个对话框还带着"桌面尺寸假设"（.ui 里写死的 minimumSize
	 * 或某个子控件的 minimumHeight），需要在它的构造函数里放宽 —— 见
	 * OBSBasicSourceSelect / OBSBasicSettings 的 #ifdef __ANDROID__ 块。
	 *
	 * 踩过的三层留档，免得以后再摸一遍：
	 *   ① .ui 里写死 minimumHeight（添加源的 sourceTypeList 是 460）；
	 *   ② 布局首次 show 时把窗口自己的 minimumSize 设成布局最小尺寸
	 *      （QLayout 默认 SetDefaultConstraint），QWidget::resize() 被它夹住；
	 *   ③ QLayout 算最小尺寸走 qSmartMinSize()（qlayoutengine.cpp:321-333）：
	 *      sizePolicy 没有 ShrinkFlag 时取 max(sizeHint, minimumSizeHint)。
	 * ②③ 都能用"在源码里放宽最小尺寸"绕开，不需要去碰这些机制。 */
	if (r.actual != r.target) {
		/* 诊断：找出最占高度的那个子控件，够定位了（不再排前 5 名）。 */
		QString worst;
		int worstH = 0;
		for (QWidget *c : w->findChildren<QWidget *>()) {
			const int h = c->minimumSizeHint().height();
			if (h > worstH) {
				worstH = h;
				worst = QStringLiteral("%1/%2 minH=%3 sizeHint=%4")
						.arg(QString::fromLatin1(c->metaObject()->className()),
						     c->objectName())
						.arg(c->minimumHeight())
						.arg(c->sizeHint().height());
			}
		}

		/* 这里**故意不再硬压**。以前用 setMinimumSize(0,0)+SetNoConstraint 把窗口
		 * 强行缩到目标尺寸，会造出"窗口比内容小"的不一致状态：布局仍按原来的最小
		 * 尺寸摆放子控件，控件被裁在窗口外 —— 实测把添加源列表的滚动和命中判定都
		 * 搞坏了（列表 viewport 比可视区大，滚动条范围算错）。
		 *
		 * 正确做法是在对话框自己的构造函数里放宽最小尺寸，参考
		 * OBSBasicSourceSelect / OBSBasicSettings 里的 #ifdef __ANDROID__ 块。
		 * 下面这行就是待办清单：出现了就说明该对话框还没做源码级修复。 */
		obsAndroidDisplayLog(
			"CLAMP-BLOCKED %s：目标 %dx%d 实际 %dx%d；最占高度=%s —— 该对话框需要在源码里放宽最小尺寸",
			w->metaObject()->className(), r.target.width(), r.target.height(), r.actual.width(),
			r.actual.height(), worst.toUtf8().constData());
	}

	obsAndroidDisplayLog("CLAMP %s %dx%d -> 目标 %dx%d 实际 %dx%d（可用 %dx%d）", w->metaObject()->className(),
			     r.before.width(), r.before.height(), r.target.width(), r.target.height(),
			     r.actual.width(), r.actual.height(), r.avail.width(), r.avail.height());
}

/* 为滚动区启用单指触摸滑动（QScroller + AndroidTouchScrollGuard）。
 *
 * 不直接挂 QScroller::grabGesture(LeftMouseButtonGesture) + MousePressEventDelay：
 *   Qt 6 内置单例 PressDelayHandler::sendingEvent 在派发单击触发的模态循环
 *   （例如点控制按钮里的“设置”进入 OBSBasicSettings::exec()）期间全程保持 true，
 *   会导致弹窗内的所有 QScroller 被全局 shouldEventBeIgnored() 吞掉而无法滑动。
 *   改为仅配置 QScroller 实例，统一由 AndroidTouchScrollGuard 负责 150ms 延迟按下、
 *   拖拽防误触、以及 QSlider 垂直/水平方向分流；同时关闭越界回弹移位。 */
static bool canScrollAxis(const QScrollBar *bar)
{
	return bar && bar->maximum() > bar->minimum();
}

static bool canScrollAreaAnyDirection(const QAbstractScrollArea *area)
{
	return area && (canScrollAxis(area->horizontalScrollBar()) || canScrollAxis(area->verticalScrollBar()));
}

static bool isAndroidScrollTargetAllowed(const QWidget *area)
{
	if (!area)
		return false;
	if (const auto *itemView = qobject_cast<const QAbstractItemView *>(area)) {
		// Preserve single-finger drag-and-drop reordering in lists like Scenes and Sources.
		if (itemView->dragEnabled() && itemView->dragDropMode() != QAbstractItemView::NoDragDrop)
			return false;
	}
	const QWidget *win = area->window();
	if (!win)
		return true;
	const Qt::WindowFlags f = win->windowFlags();
	if (f.testFlag(Qt::ToolTip) || win->inherits("QDockWidgetGroupWindow"))
		return false;
	if (f.testFlag(Qt::Popup)) {
		const auto *combo = qobject_cast<const QComboBox *>(win->parentWidget());
		return combo && combo->view()->window() == win;
	}
	return true;
}

static bool enableFlickScrollOnArea(QAbstractScrollArea *area)
{
	if (!isAndroidScrollTargetAllowed(area))
		return false;

	QWidget *vp = area->viewport();
	if (!vp || QScroller::hasScroller(vp))
		return false;

	if (auto *itemView = qobject_cast<QAbstractItemView *>(area)) {
		itemView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
		itemView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
	}

	QScroller *scroller = QScroller::scroller(vp);
	if (!scroller)
		return false;
	QScrollerProperties props = scroller->scrollerProperties();
	props.setScrollMetric(QScrollerProperties::DragStartDistance, 0.002);
	props.setScrollMetric(QScrollerProperties::MousePressEventDelay, 0.0);
	props.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
	props.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
	scroller->setScrollerProperties(props);
	return true;
}

static void enableFlickScroll(QWidget *w)
{
	if (!isAndroidScrollTargetAllowed(w))
		return;

	const QList<QAbstractScrollArea *> areas = w->findChildren<QAbstractScrollArea *>();
	int grabbed = 0;
	int itemViews = 0;
	for (QAbstractScrollArea *area : areas) {
		if (enableFlickScrollOnArea(area)) {
			++grabbed;
			if (qobject_cast<QAbstractItemView *>(area) != nullptr)
				++itemViews;
		}
	}

	if (grabbed > 0) {
		obsAndroidDisplayLog("FLICK %s：滚动区 %d 个，本次开 %d 个（其中 item view %d 个）",
				     w->metaObject()->className(), int(areas.size()), grabbed, itemViews);
	}
}

static void fitPopupToSafeArea(QWidget *popup, const QWidget *anchor, int paddingPx, bool clampSize)
{
	if (!popup || !popup->isVisible())
		return;
	const QWidget *ref = anchor ? anchor : popup;
	const QScreen *scr = ref->screen() ? ref->screen()
					   : (ref->window() ? ref->window()->screen() : QGuiApplication::primaryScreen());
	if (!scr)
		return;
	QRect available = scr->availableGeometry();
	const QWindow *host = nullptr;
	if (anchor && anchor->window())
		host = anchor->window()->windowHandle();
	else if (popup->windowHandle() && popup->windowHandle()->transientParent())
		host = popup->windowHandle()->transientParent();
	else if (QWidget *active = QApplication::activeWindow())
		host = active->windowHandle();
	if (host) {
		// Dialogs and menus can open near screen edges or in narrow multi-window bounds.
		while (host->transientParent()) {
			host = host->transientParent();
		}
		const QMargins safe = obsAndroidWindowSafeMargins(host, scr);
		available = available.intersected(host->geometry().marginsRemoved(safe));
	} else {
		const QMargins safe = obsAndroidWindowSafeMargins(nullptr, scr);
		available = available.marginsRemoved(safe);
	}
	if (paddingPx > 0)
		available = available.marginsRemoved(QMargins(paddingPx, paddingPx, paddingPx, paddingPx));
	if (available.isEmpty())
		return;

	QRect geometry = popup->geometry();
	if (clampSize)
		geometry.setSize(geometry.size().boundedTo(available.size()));
	if (geometry.width() <= available.width()) {
		geometry.moveLeft(qBound(available.left(), geometry.left(), available.right() - geometry.width() + 1));
	} else {
		geometry.moveLeft(available.left());
	}
	if (geometry.height() <= available.height()) {
		geometry.moveTop(qBound(available.top(), geometry.top(), available.bottom() - geometry.height() + 1));
	} else {
		geometry.moveTop(available.top());
	}
	if (geometry != popup->geometry())
		popup->setGeometry(geometry);
}

static void fitComboBoxPopup(QWidget *popup, const QComboBox *combo)
{
	fitPopupToSafeArea(popup, combo, 8, true);

	QAbstractItemView *view = combo->view();
	enableFlickScrollOnArea(view);
	view->scrollTo(view->currentIndex());
}

class AndroidTouchScrollGuard : public QObject {
public:
	static AndroidTouchScrollGuard *&globalSlot()
	{
		static AndroidTouchScrollGuard *guard = nullptr;
		return guard;
	}

	static AndroidTouchScrollGuard *current() { return globalSlot(); }

	bool isCancelRelease(const QEvent *e) const
	{
		return cancelingScroll && e && e->type() == QEvent::MouseButtonRelease;
	}

	explicit AndroidTouchScrollGuard(QObject *parent) : QObject(parent)
	{
		monotonicClock.start();
		holdTimer.setSingleShot(true);
		connect(&holdTimer, &QTimer::timeout, this, [this] {
			if (pendingTarget && qApp)
				promotePendingToWidget(static_cast<QApplication *>(qApp));
		});
	}

	~AndroidTouchScrollGuard() override
	{
		if (globalSlot() == this)
			globalSlot() = nullptr;
	}

	bool beforeNotify(QApplication *app, QObject *receiver, QEvent *e, bool &outResult)
	{
		if (replayingEvent == e)
			return false;

		const QEvent::Type type = e->type();
		if (type == QEvent::ApplicationStateChange) {
			if (QGuiApplication::applicationState() != Qt::ApplicationActive)
				reset();
			return false;
		}
		if (type == QEvent::WindowDeactivate) {
			QWidget *trackedWin = activeWindow();
			if (trackedWin && receiver == trackedWin)
				reset();
			return false;
		}
		if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick &&
		    type != QEvent::MouseMove && type != QEvent::MouseButtonRelease)
			return false;

		auto *me = static_cast<QMouseEvent *>(e);

		if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick)
			return handlePress(me, receiver, outResult);
		if (type == QEvent::MouseMove)
			return handleMove(app, me, outResult);
		return handleRelease(app, receiver, me, outResult);
	}

	void afterNotify(QEvent *e)
	{
		if (replayingEvent == e)
			return;
		if (e->type() == QEvent::MouseButtonRelease) {
			activeHoldTarget.clear();
			postHoldTarget.clear();
			postHoldArea.clear();
			postHoldViewport.clear();
		}
	}

private:
	static constexpr int HOLD_DELAY_MS = 150;
	static constexpr int AXIS_SLOP_LOGICAL_PX = 10;
	static constexpr int POST_HOLD_SLOP_LOGICAL_PX = 18;

	template<typename T> struct ScopedAssign {
		T &slot;
		T prev;
		explicit ScopedAssign(T &s, T v) : slot(s), prev(s) { slot = v; }
		~ScopedAssign() { slot = prev; }
	};

	QWidget *activeWindow() const
	{
		QWidget *w = pendingTarget
				     ? pendingTarget.data()
				     : (postHoldTarget ? postHoldTarget.data()
						       : (activeHoldTarget ? activeHoldTarget.data()
									   : scrollerTarget.data()));
		return w ? w->window() : nullptr;
	}

	static QWidget *findScrollTarget(QWidget *leaf, QAbstractScrollArea **outArea)
	{
		if (outArea)
			*outArea = nullptr;
		if (!leaf)
			return nullptr;
		if (QWidget *pop = QApplication::activePopupWidget(); pop && !pop->isAncestorOf(leaf) && pop != leaf)
			return nullptr;
		for (QWidget *cur = leaf; cur; cur = cur->isWindow() ? nullptr : cur->parentWidget()) {
			auto *area = qobject_cast<QAbstractScrollArea *>(cur->parentWidget());
			if (!area || area->viewport() != cur)
				continue;
			if (!isAndroidScrollTargetAllowed(area))
				return nullptr;
			if (QScroller::hasScroller(cur) && canScrollAreaAnyDirection(area)) {
				if (outArea)
					*outArea = area;
				return cur;
			}
		}
		return nullptr;
	}

	static bool canHoldTransitionToScroll(const QWidget *target, const QAbstractScrollArea *area)
	{
		if (!target || qobject_cast<const QAbstractItemView *>(area))
			return false;
		return !qobject_cast<const QSlider *>(target) && !qobject_cast<const QComboBox *>(target) &&
		       !qobject_cast<const QAbstractSpinBox *>(target) && !qobject_cast<const QLineEdit *>(target) &&
		       !qobject_cast<const QTextEdit *>(target) && !qobject_cast<const QPlainTextEdit *>(target);
	}

	static std::unique_ptr<QMouseEvent> cloneMouseEvent(const QMouseEvent *me)
	{
		return std::unique_ptr<QMouseEvent>(static_cast<QMouseEvent *>(me->clone()));
	}

	void dispatchReplayedEvent(QApplication *app, QWidget *target, QEvent *ev)
	{
		ScopedAssign<QEvent *> scope(replayingEvent, ev);
		app->QApplication::notify(target, ev);
	}

	void clearPending()
	{
		holdTimer.stop();
		heldPress.reset();
		pendingTarget.clear();
		pendingSlider.clear();
		pendingArea.clear();
		pendingViewport.clear();
	}

	bool handlePress(const QMouseEvent *me, QObject *receiver, bool &outResult)
	{
		if (me->button() != Qt::LeftButton)
			return false;
		reset();

		auto *widget = qobject_cast<QWidget *>(receiver);
		if (!widget)
			return false;

		QAbstractScrollArea *area = nullptr;
		QWidget *vp = findScrollTarget(widget, &area);
		if (!area || !vp)
			return false;

		QScroller *scroller = QScroller::scroller(vp);
		if (scroller->state() != QScroller::Inactive) {
			const bool consumed = scroller->handleInput(QScroller::InputPress,
								    vp->mapFromGlobal(me->globalPosition()),
								    monotonicClock.elapsed());
			if (consumed && scroller->state() != QScroller::Inactive) {
				scrollerTarget = vp;
				outResult = true;
				return true;
			}
			if (scroller->state() != QScroller::Inactive)
				scroller->stop();
		}

		if (auto *slider = qobject_cast<QSlider *>(widget); slider && slider->isEnabled()) {
			const bool canScrollPerp =
				(slider->orientation() == Qt::Horizontal)
					? canScrollAxis(area->verticalScrollBar())
					: canScrollAxis(area->horizontalScrollBar());
			if (!canScrollPerp) {
				activeHoldTarget = slider;
				return false;
			}
			pendingSlider = slider;
		}

		pendingTarget = widget;
		pendingArea = area;
		pendingViewport = vp;
		pressGlobal = me->globalPosition().toPoint();
		pressGlobalF = me->globalPosition();
		pressElapsedMs = monotonicClock.elapsed();
		heldPress = cloneMouseEvent(me);
		holdTimer.start(HOLD_DELAY_MS);
		outResult = true;
		return true;
	}

	bool handleMove(QApplication *app, const QMouseEvent *me, bool &outResult)
	{
		if (activeHoldTarget)
			return false;
		if (scrollerTarget) {
			QScroller::scroller(scrollerTarget)
				->handleInput(QScroller::InputMove,
					      scrollerTarget->mapFromGlobal(me->globalPosition()),
					      monotonicClock.elapsed());
			outResult = true;
			return true;
		}
		if (pendingTarget) {
			if (!pendingViewport || !heldPress) {
				reset();
				return false;
			}
			const QPoint delta = me->globalPosition().toPoint() - pressGlobal;
			const int dx = qAbs(delta.x());
			const int dy = qAbs(delta.y());
			if (dx + dy < AXIS_SLOP_LOGICAL_PX) {
				outResult = true;
				return true;
			}
			if (pendingSlider && pendingSlider->isEnabled()) {
				const bool alongSlider =
					(pendingSlider->orientation() == Qt::Horizontal) ? (dx >= dy) : (dy >= dx);
				if (alongSlider) {
					promotePendingToWidget(app);
					return false;
				}
			}
			if (!promotePendingToScroller(me)) {
				promotePendingToWidget(app);
				return false;
			}
			outResult = true;
			return true;
		}
		if (postHoldTarget) {
			if (!postHoldViewport || !postHoldArea || QApplication::activePopupWidget()) {
				postHoldTarget.clear();
				postHoldArea.clear();
				postHoldViewport.clear();
				return false;
			}
			const QPoint delta = me->globalPosition().toPoint() - pressGlobal;
			if (qAbs(delta.x()) + qAbs(delta.y()) < POST_HOLD_SLOP_LOGICAL_PX)
				return false;
			if (promotePostHoldToScroller(app, me)) {
				outResult = true;
				return true;
			}
			return false;
		}
		return false;
	}

	bool handleRelease(QApplication *app, QObject *receiver, const QMouseEvent *me, bool &outResult)
	{
		if (scrollerTarget) {
			QPointer<QWidget> vp = scrollerTarget;
			scrollerTarget.clear();
			if (vp) {
				QScroller::scroller(vp)->handleInput(QScroller::InputRelease,
								     vp->mapFromGlobal(me->globalPosition()),
								     monotonicClock.elapsed());
			}
			outResult = true;
			return true;
		}
		if (pendingTarget) {
			QPointer<QWidget> target = pendingTarget;
			QPointer<QObject> recGuard(receiver);
			auto press = std::move(heldPress);
			clearPending();
			if (target && press)
				dispatchReplayedEvent(app, target, press.get());
			if (!recGuard) {
				outResult = true;
				return true;
			}
			return false;
		}
		return false;
	}

	void promotePendingToWidget(QApplication *app)
	{
		QPointer<QWidget> target = pendingTarget;
		QPointer<QAbstractScrollArea> area = pendingArea;
		QPointer<QWidget> vp = pendingViewport;
		auto press = std::move(heldPress);
		clearPending();
		if (!target)
			return;
		if (canHoldTransitionToScroll(target, area) && vp) {
			postHoldTarget = target;
			postHoldArea = area;
			postHoldViewport = vp;
		} else {
			activeHoldTarget = target;
		}
		if (press)
			dispatchReplayedEvent(app, target, press.get());
	}

	bool startScrollerDrag(QWidget *vp, QAbstractSlider *slider, const QPointF &moveGlobalPos)
	{
		if (!vp)
			return false;
		if (!slider) {
			if (QWidget *child = vp->childAt(vp->mapFromGlobal(pressGlobalF.toPoint()))) {
				if (!qobject_cast<QScrollBar *>(child))
					slider = qobject_cast<QAbstractSlider *>(child);
			}
		}
		QScroller *scroller = QScroller::scroller(vp);
		QPointer<QAbstractSlider> sliderGuard(slider);
		const bool wasTransparent = slider && slider->testAttribute(Qt::WA_TransparentForMouseEvents);
		// QAbstractScrollAreaPrivate::canStartScrollingAt rejects ScrollPrepare over a QAbstractSlider.
		if (slider && !wasTransparent)
			slider->setAttribute(Qt::WA_TransparentForMouseEvents, true);
		scroller->handleInput(QScroller::InputPress, vp->mapFromGlobal(pressGlobalF), pressElapsedMs);
		if (sliderGuard && !wasTransparent)
			sliderGuard->setAttribute(Qt::WA_TransparentForMouseEvents, false);
		if (scroller->state() == QScroller::Inactive)
			return false;
		scrollerTarget = vp;
		scroller->handleInput(QScroller::InputMove, vp->mapFromGlobal(moveGlobalPos),
				      monotonicClock.elapsed());
		return true;
	}

	bool promotePendingToScroller(const QMouseEvent *moveEvent)
	{
		if (!startScrollerDrag(pendingViewport, pendingSlider, moveEvent->globalPosition()))
			return false;
		clearPending();
		return true;
	}

	bool promotePostHoldToScroller(QApplication *app, const QMouseEvent *moveEvent)
	{
		QPointer<QWidget> target = postHoldTarget;
		QPointer<QWidget> vp = postHoldViewport;
		postHoldTarget.clear();
		postHoldArea.clear();
		postHoldViewport.clear();
		if (target) {
			// Cancel button highlight and long-press timer without triggering clicked() or context menus.
			const QPointF farAway(-1e6, -1e6);
			QMouseEvent cancelRelease(QEvent::MouseButtonRelease, farAway, farAway, farAway, Qt::LeftButton,
						  Qt::NoButton, moveEvent->modifiers(), moveEvent->source(),
						  moveEvent->pointingDevice());
			cancelRelease.setTimestamp(moveEvent->timestamp());
			ScopedAssign<bool> cancelScope(cancelingScroll, true);
			dispatchReplayedEvent(app, target, &cancelRelease);
		}
		return startScrollerDrag(vp, nullptr, moveEvent->globalPosition());
	}

	void reset()
	{
		clearPending();
		activeHoldTarget.clear();
		postHoldTarget.clear();
		postHoldArea.clear();
		postHoldViewport.clear();
		if (scrollerTarget) {
			QScroller *scroller = QScroller::scroller(scrollerTarget);
			if (scroller && scroller->state() == QScroller::Dragging)
				scroller->stop();
		}
		scrollerTarget.clear();
	}

	QTimer holdTimer;
	QElapsedTimer monotonicClock;
	QPointer<QWidget> pendingTarget;
	QPointer<QSlider> pendingSlider;
	QPointer<QAbstractScrollArea> pendingArea;
	QPointer<QWidget> pendingViewport;
	QPointer<QWidget> activeHoldTarget;
	QPointer<QWidget> postHoldTarget;
	QPointer<QAbstractScrollArea> postHoldArea;
	QPointer<QWidget> postHoldViewport;
	QPointer<QWidget> scrollerTarget;
	QPoint pressGlobal;
	QPointF pressGlobalF;
	qint64 pressElapsedMs = 0;
	std::unique_ptr<QMouseEvent> heldPress;
	QEvent *replayingEvent = nullptr;
	bool cancelingScroll = false;
};

class AndroidInputFilter : public QObject {
public:
	explicit AndroidInputFilter(QObject *parent = nullptr) : QObject(parent)
	{
		timer.setSingleShot(true);
		/* 到点只是"长按到位"，不当场弹菜单 —— OBS 的上下文菜单结尾是阻塞的 popup.exec()，
		 * 手指还按着的时候弹出去，会被随后那发 touch 当成"点在菜单外"立刻 dismiss
		 * （2026-09-05 20:36 实测：sendEvent 同毫秒返回，一句 setVisible(false) 都没有）。
		 * 真正的弹出发放延到松手那一刻。 */
		connect(&timer, &QTimer::timeout, this, [this] { armed = true; });
	}

protected:
	bool eventFilter(QObject *watched, QEvent *ev) override
	{
		if (!watched->isWidgetType())
			return false;

		switch (ev->type()) {
		/* Android 的返回键 = Escape。这一条是"先量再修"量出来的，而且**推翻了动手前的猜测**：
		 * BACK 不是没送进 Qt —— build 73 实测（`.qoder/back-measure.sh`、留档 `.qoder/backprobe/`）
		 * 它以 QEvent::KeyPress、key=Qt::Key_Back(0x1000061，qnamespace.h:855)、nativeVirtualKey=0
		 * 正常进来，沿焦点链一路传播到底没人接（主界面那一拍 6 个控件各打一行、投影仪那一拍只到
		 * OBSProjector），而**同一个窗**上紧接着发 ESCAPE(111)=Qt::Key_Escape(0x1000000) 就被
		 * OBSProjector.cpp:63 那条 QAction 接走并关掉了窗 ⇒ 缺的只有"翻译"这一环：前端全树对
		 * Qt::Key_Back 零处理（grep frontend/ 命中 0）。
		 * 为什么换成 Escape 而不是直接 close()/reject()：投影仪的关闭链是
		 * EscapeTriggered → OBSBasic::DeleteProjector（键盘 :65 与触摸菜单 :310 两条路都走它），
		 * 自己 close() 会绕过它；而根界面上浮动 dock 之类也是顶层窗，close() 会把它直接收掉。
		 * 送 Escape 就等于把"桌面上按 Escape 会发生什么"原样搬过来：QDialog 靠 app_event_filter:266
		 * 放行 + 它自己的 keyPressEvent reject，投影仪/Multiview 靠那条快捷键。 */
		case QEvent::KeyPress: {
			QKeyEvent *ke = static_cast<QKeyEvent *>(ev);
			if (ke->key() != Qt::Key_Back)
				break;
			/* 菜单/popup 立着时 Qt 自己就把它撤了（09-07 00:26 实测 BACK 能关 popup 菜单），不抢。 */
			if (QApplication::activePopupWidget())
				break;
			QWidget *top = static_cast<QWidget *>(watched)->window();
			/* 焦点还在根界面上 ⇒ BACK 留给 Android。今天它在主界面上什么都不做（01:39 实测
			 * pid 不变、chrome 仍 39,42,51），"退回后台"是另一笔账，不混进这一条里改。 */
			if (top == static_cast<OBSApp *>(qApp)->GetMainWindow())
				break;
			backEaten = true;
			QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
			QApplication::sendEvent(top, &esc);
			obsAndroidDisplayLog("BACK→Escape：%s（模态=%d 送出后 accepted=%d）", top->metaObject()->className(),
					     (int)top->isModal(), (int)esc.isAccepted());
			return true;
		}
		case QEvent::KeyRelease:
			/* 按下那拍已经当成 Escape 用掉了；留着这次松手只会再走一遍"BACK 没人处理"。 */
			if (backEaten && static_cast<QKeyEvent *>(ev)->key() == Qt::Key_Back) {
				backEaten = false;
				return true;
			}
			break;
		/* ===== P-8 探针（先量再修）：组合框弹层在合成触摸下打不开 ==================================
		 * 要分开的两种长相，在截图上是同一张脸（都是"两秒后屏上没有弹层"）：
		 *   A 从未 Show ⇒ `QComboBox::mousePressEvent` 里那条 `d->popup()` 根本没跑到（事件没进来 / 被手势
		 *     判走 / 落点不是那个控件）；
		 *   B Show 了又立刻 Hide ⇒ 弹层立起来又被打死，看 Hide 隔了多少毫秒、松手落在谁身上。
		 * 四行各管一环：P8-PRESS（按下落到谁、是不是 QComboBox）、P8-G（有没有被判成 TapAndHold 手势）、
		 * P8-W（顶层窗 Show/Hide/Close/Expose 与几何）、P8-REL（松手时谁是 activePopupWidget）。
		 * 前缀一律 ASCII —— 设备端 grep 中文不可靠（仪器账第 7 次）。 */
		case QEvent::Show:
		case QEvent::Hide:
		case QEvent::Close:
		case QEvent::Expose: {
			if (ev->type() == QEvent::Show) {
				if (auto *area = qobject_cast<QAbstractScrollArea *>(watched)) {
					QTimer::singleShot(0, area, [area] { enableFlickScrollOnArea(area); });
				}
			}
			QWidget *pw = static_cast<QWidget *>(watched);
			if (!pw->isWindow())
				break;
			/* 主窗不打 P8-W/CLAMP 日志：它的 Expose 每轮都来；但 Show 时要给它挂单指滑动。 */
			if (pw == static_cast<OBSApp *>(qApp)->GetMainWindow()) {
				if (ev->type() == QEvent::Show) {
					QTimer::singleShot(0, pw, [pw] { enableFlickScroll(pw); });
				}
				break;
			}
			const Qt::WindowFlags flags = pw->windowFlags();
			const QRect geo = pw->geometry();
			const char *kind = ev->type() == QEvent::Show
						   ? "SHOW"
						   : ev->type() == QEvent::Hide
							     ? "HIDE"
							     : ev->type() == QEvent::Close ? "CLOSE" : "EXPOSE";
			obsAndroidDisplayLog(
				"P8-W %s cls=%s obj=%s geo=%dx%d+%d+%d popup=%d tool=%d dialog=%d vis=%d act=%d", kind,
				pw->metaObject()->className(), pw->objectName().toUtf8().constData(), geo.x(), geo.y(),
				geo.width(), geo.height(), int(flags.testFlag(Qt::Popup)), int(flags.testFlag(Qt::Tool)),
				int(flags.testFlag(Qt::Dialog)), int(pw->isVisible()),
				int(qApp->activePopupWidget() == pw));

			/* 等 Qt 完成布局后，约束下拉框与上下文菜单弹层位置，或调整普通对话框并启用滚动。 */
			if (ev->type() == QEvent::Show) {
				QTimer::singleShot(0, pw, [pw] {
					if (pw->windowType() == Qt::Popup) {
						const auto *combo = qobject_cast<const QComboBox *>(pw->parentWidget());
						if (combo && pw->isVisible() && combo->view()->window() == pw) {
							fitComboBoxPopup(pw, combo);
						} else if (qobject_cast<QMenu *>(pw) && pw->isVisible()) {
							fitPopupToSafeArea(pw, pw->parentWidget(), 0, false);
						}
						return;
					}
					clampTopLevelToScreen(pw);
					enableFlickScroll(pw);
				});
			}
			break;
		}
		case QEvent::Gesture: {
			QGestureEvent *ge = static_cast<QGestureEvent *>(ev);
			if (QGesture *g = ge->gesture(Qt::TapAndHoldGesture))
				obsAndroidDisplayLog("P8-G tapandhold state=%d recv=%s accepted=%d", (int)g->state(),
						       watched->metaObject()->className(), int(ev->isAccepted()));
			break;
		}
		case QEvent::MouseButtonPress: {
			QWidget *pw = static_cast<QWidget *>(watched);
			const QRect geo = pw->geometry();
			obsAndroidDisplayLog("P8-PRESS cls=%s obj=%s geo=%dx%d+%d+%d combo=%d win=%s",
					     pw->metaObject()->className(), pw->objectName().toUtf8().constData(), geo.x(),
					     geo.y(), geo.width(), geo.height(),
					     int(qobject_cast<QComboBox *>(pw) != nullptr),
					     pw->window() ? pw->window()->metaObject()->className() : "?");
			QMouseEvent *me = static_cast<QMouseEvent *>(ev);
			if (me->button() != Qt::LeftButton)
				break;
			/* P-8 修法 v3（组合框）。b83 实测（.qoder/p8-b83/p8.txt）：弹层从来不是"打不开"，
			 * 而是"开在松手之前的 1 ms 里、又被这发松手关掉"——
			 *   41.811 PRESS→QComboBox「simpleOutRecFormat」 / 41.815 SHOW→QComboBoxPrivateContainer
			 *   / 41.816 REL（落到主窗那个 QWidget）+ HIDE，**同一毫秒**。
			 * 同一处按满 400 ms（G2）弹层就活得好好的，截图 G2-combo.png 里两档混合容器都立着
			 * ⇒ 弹层本身能建、能画、能收后续点击，坏的只有"这发太快的松手"：
			 *   `QComboBoxPrivateContainer::eventFilter` 对落在弹层外的 MouseButtonRelease 一律 hidePopup，
			 *   而 Android 上这发松手送到的是主窗的 widget，不是刚建出来的弹层。
			 * ⇒ 最小干预：**不替 Qt 弹**（b83 证明 Qt 自己会弹），只记下"这发按下落在组合框上"，
			 *   到松手那一刻再看弹层立没立着（见下面 release 分支）。
			 * 为什么不在这里判定：v1 就是在这儿读 `activePopupWidget()` 想确认"弹起来了"，实测恒为 null
			 *   （Qt 登记活动弹层要到下一轮事件循环，SHOW 行里的 act=0 是同一件事）⇒ 判定必须放到松手时。
			 * 弹层已经立着时不记：那一下该由 Qt 的"点外面就关"收尾，不抢。
			 * 长按那套（armed/timer）这里一并跳过：弹层刚立起来，再补一发合成右键只会把它按死。 */
			if (QComboBox *cb = qobject_cast<QComboBox *>(pw)) {
				if (!qApp->activePopupWidget())
					comboPress = cb;
				break;
			}
			target = static_cast<QWidget *>(watched);
			pressGlobal = me->globalPosition().toPoint();
			armed = false;
			timer.start(HOLD_MS);
			break;
		}
		case QEvent::MouseMove: {
			QMouseEvent *me = static_cast<QMouseEvent *>(ev);
			if ((me->globalPosition().toPoint() - pressGlobal).manhattanLength() > MOVE_SLOP) {
				timer.stop(); // 这是拖动/滚动，不是长按
				armed = false;
			}
			break;
		}
		case QEvent::MouseButtonRelease:
		case QEvent::MouseButtonDblClick:
			if (ev->type() == QEvent::MouseButtonRelease) {
				if (auto *guard = AndroidTouchScrollGuard::current();
				    guard && guard->isCancelRelease(ev)) {
					timer.stop();
					armed = false;
					comboPress = nullptr;
					target.clear();
					return false;
				}
				QMouseEvent *re = static_cast<QMouseEvent *>(ev);
				QWidget *rp = static_cast<QWidget *>(watched);
				QWidget *pop = qApp->activePopupWidget();
				obsAndroidDisplayLog("P8-REL cls=%s at=(%d,%d) actPopup=%s eaten=%d", rp->metaObject()->className(),
							     re->globalPosition().toPoint().x(), re->globalPosition().toPoint().y(),
							     pop ? pop->metaObject()->className() : "none", int(comboPress != nullptr));
			}
			/* P-8 修法 v3（接上面 press 分支）：这发松手如果正好是"刚落在组合框上的那次按下"的尾巴、
			 * 而此刻弹层确实立着，就把它吃掉 —— Qt 那条"松手落在弹层外 ⇒ hidePopup"收不到事件，弹层留住。
			 * 判定放在这里（而不是按下时）是因为活动弹层要等下一轮事件循环才登记得上，见上面那段。
			 * 指针一进分支就清 ⇒ 最多多吃一发，绝不会波及后面不相干的松手（拖滚动条那类）。 */
			if (comboPress) {
				QWidget *pop = qApp->activePopupWidget();
				comboPress = nullptr;
				if (pop && pop->inherits("QComboBoxPrivateContainer")) {
					obsAndroidDisplayLog("P8-EAT 忽略本次触摸释放，保持下拉框打开：%s", pop->metaObject()->className());
					return true;
				}
			}
			timer.stop();
			if (armed) {
				armed = false;
				/* 只有真的弹起了菜单才吞这发松手。吞的理由（对列表那类成立）：既不给它补一次
				 * "点击"，也不让它以为按键还开着。但**没弹出任何东西的长按**本来就是"一次按得
				 * 久一点的点击" —— 一律吞掉就把只有 `clicked` 一个入口的按钮判了死刑（转场 "+"：
				 * 03:45 实测长按 1.2 s 三发全 `菜单立过=0`，而同一处一发 tap 就弹菜单）。 */
				if (!fire())
					return false;
				return true;
			}
			break;
		default:
			break;
		}
		return false;
	}

private:
	static constexpr int HOLD_MS = 500;
	static constexpr int MOVE_SLOP = 24;

	/* 返回值 = "这发长按真的立起过菜单吗"。松手分支要用（见 eventFilter 里 MouseButtonRelease
	 * 那段）：立过才吞松手，没立就把它交回控件 —— 否则只有 `clicked` 一个入口的按钮
	 * （转场 "+" 就是）在手指按久一点时变成"点了没反应"。03:45 实测：三条手势形态各长按 1.2 s，
	 * `锚「transitionAdd」` 一次都没新增、`菜单立过=0`。三条出口各自量耗时 > 0 就算立过 ——
	 * OBS 的菜单结尾是阻塞的 popup.exec()，同毫秒返回的那条路根本没弹东西。 */
	bool fire()
	{
		QWidget *w = target.data();
		if (!w)
			return false;

		/* 按下事件落在的是 QAbstractScrollArea 的 viewport，而策略设在列表控件自己身上；
		 * 往上找到那个声明了策略的祖先，pos 也按它的坐标系给 —— OBS 的 handler 里是 itemAt(pos)。
		 * build 80 在这条向下的路之前先补一次**向下的几何落点**：04:42 实测预览区那发按下的接收者
		 * 是**顶层窗自己**（`w「OBSBasic」1067x667+0+0 顶层=1`），而 `preview` 那条
		 * `Qt::CustomContextMenu` 设在它**自己身上**（`forms/OBSBasic.ui:240`）—— 既不在 w 上、
		 * 也不在 w 的祖先里 ⇒ 只往上爬永远找不到它。`childAt` 是纯几何的下钻（不看事件派发怎么
		 * 选接收者），拿同一个 pressGlobal 在 w 的子树里再落一次点。**只在往下落空了之后才走老路**，
		 * 且老路 `policyUp(w->parentWidget())` 与 build 79 逐字同形 ⇒ 现在绿着的那些腿一步没动。 */
		auto policyUp = [](QWidget *x) {
			while (x) {
				const Qt::ContextMenuPolicy policy = x->contextMenuPolicy();
				if (policy == Qt::CustomContextMenu || policy == Qt::ActionsContextMenu)
					return x;
				x = x->parentWidget();
			}
			return static_cast<QWidget *>(nullptr);
		};
		QWidget *const hit = w->childAt(w->mapFromGlobal(pressGlobal));
		QWidget *menu = hit ? policyUp(hit) : nullptr;
		if (!menu)
			menu = policyUp(w->parentWidget());
		if (!menu)
			menu = w;

		/* 给 3-4 探针一个能 grep 的确定信号：光看"菜单有没有弹"分不清是长按没触发，
		 * 还是触发了但那个控件压根没挂菜单。
		 * 行尾那三个字段是 04:1x 之后加的：只有 className 时"接收=OBSBasic"这一格无法自证 ——
		 * 预览中心那三个落点报的就是 OBSBasic，可落点明明在显示面自己的那个原生子窗（#7）里。
		 * ⇒ 补 objectName（同一张 .ui 里独一份）、它在父系里的几何、它自己是不是顶层窗、
		 *   以及它所属顶层窗的 objectName。**一律追加在行尾**，前面 `%s@(%d,%d) → %s` 那截不动 ——
		 *   `.qoder/longpress-hit-probe.sh` 与 `menupos-probe.sh` 的正则都吃这个前缀。 */
		const QRect wGeo = w->geometry();
		const QWidget *wWin = w->window();
		obsAndroidDisplayLog("3-4 长按→右键：%s@(%d,%d) → %s %s〔w「%s」%dx%d+%d+%d 顶层=%d 属于「%s」〕",
				     w->metaObject()->className(), pressGlobal.x(), pressGlobal.y(),
				     menu->metaObject()->className(),
				     menu == w ? "（未找到菜单策略祖先，投给事件原点）" : "",
				     qUtf8Printable(w->objectName()), wGeo.width(), wGeo.height(), wGeo.x(),
				     wGeo.y(), int(w->isWindow()), qUtf8Printable(wWin ? wWin->objectName() : QString()));

		/* 单独一行、不动上面那个前缀：记下"向下的几何落点找到了谁、最后采用的是谁"。
		 * 这一格是 build 80 那次改动唯一的判据来源 —— 没有它，"菜单起来了"和"菜单为什么
		 * 起来了"在日志里是同一张脸。 */
		const QString hitDesc = hit ? QStringLiteral("「%1」%2").arg(hit->objectName(),
									     QString::fromUtf8(
										     hit->metaObject()->className()))
					    : QStringLiteral("空");
		const QByteArray hitUtf8 = hitDesc.toUtf8();
		const QByteArray tailUtf8 =
			(menu == w ? QStringLiteral("（落点也没找到策略持有者）") : QString()).toUtf8();
		obsAndroidDisplayLog("3-4 长按→下钻：childAt=%s 采用=「%s」%s", hitUtf8.constData(),
				     qUtf8Printable(menu->objectName()), tailUtf8.constData());

		/* 坐标系：QAbstractScrollArea 那一类（SourceTree/SceneTree 都是）的
		 * `customContextMenuRequested` 按 Qt 文档给的是 **viewport 系**坐标 —— OBS 的 handler 里
		 * 就是 `itemAt(pos)`，而 `menu` 是带 2px 边框的外层控件。照外层算会整体偏一个边框宽：
		 * 02:32 实测手指在逻辑(160,76)、菜单却起于(162,78)；行边界上这一偏还会选错行。 */
		QWidget *coordBase = menu;
		if (QAbstractScrollArea *scroll = qobject_cast<QAbstractScrollArea *>(menu))
			coordBase = scroll->viewport();
		const QPoint local = coordBase->mapFromGlobal(pressGlobal);
		QContextMenuEvent ce(QContextMenuEvent::Mouse, local, pressGlobal);
		/* 分岔口不是 isAccepted()：QContextMenuEvent 造出来就是 accepted 的，CustomContextMenu
		 * 那条分支也只是 emit 一下就 return，谁都没碰过这个标志位 —— 20:36 那句"已受理"是假信号。
		 * 真正的判据是**耗时**：OBS 的 CreateSourcePopupMenu 结尾是阻塞的 popup.exec()，菜单只要
		 * 立起来，sendEvent 就要待到菜单关掉才返回（手指抬起时）；同毫秒返回 = 菜单压根没弹。 */
		QElapsedTimer held;
		held.start();
		QApplication::sendEvent(menu, &ce);
		const long long spent = (long long)held.elapsed();
		obsAndroidDisplayLog("3-4 长按→右键：sendEvent 返回 用时=%lldms 弹出菜单=%s", spent,
				     qApp->activePopupWidget() ? "在场" : "已关");

		if (spent > 0)
			return true; // 阻塞过 = 菜单真的立起来过
		/* 0ms 返回就补一发直发 —— 20:45 实测过：sendEvent 到 SourceTree/SceneTree 这类
		 * CustomContextMenu 控件，Android 上**不会**走到 OBSBasic 的 handler（0ms 返回、无菜单），
		 * 而 `invokeMethod("customContextMenuRequested")` 一发就中（菜单 33 项，藏面正常）。
		 * sendEvent 还是留着先走一遍：contextMenuEvent 覆写型控件（如 VolumeControl）只认事件不认信号。 */
		{
			obsAndroidDisplayLog("3-4 长按→右键：0ms 返回 → 直发 customContextMenuRequested(%d,%d)",
					     local.x(), local.y());
			/* 判据不能是 invokeMethod 的返回值：`customContextMenuRequested` 是 **QWidget**
			 * 自己声明的信号，任何 widget 上都"存在"，invokeMethod 恒返回 true —— 18:58 实测
			 * 那条 !ok 分支永远进不去，投影仪长按什么也没弹。
			 * 换成和上面同一把尺子：**耗时**。OBS 的 handler 结尾是阻塞的 popup.exec()，菜单
			 * 立起来 invokeMethod 就要等菜单关掉才返回；0ms = 信号发了但没人接。 */
			QElapsedTimer held2;
			held2.start();
			const bool ok = QMetaObject::invokeMethod(menu, "customContextMenuRequested",
								  Q_ARG(QPoint, local));
			const long long spent2 = (long long)held2.elapsed();
			if (ok && spent2 > 0) {
				obsAndroidDisplayLog("3-4 长按→右键：直发已处理 用时=%lldms（菜单弹过又关了）", spent2);
				return true;
			}
			obsAndroidDisplayLog("3-4 长按→右键：直发无人接（ok=%d 用时=%lldms）→ 补发右键到 %s",
					     ok, spent2, w->metaObject()->className());
			/* 还有一格：既不吃 QContextMenuEvent、直发信号又没人接的控件 —— `OBSProjector`
			 * 就是（它的右键菜单写在 mousePressEvent 的 RightButton 分支里，结尾同样是
			 * 阻塞的 popup.exec()）。Android 上顶层窗没有标题栏、KEYCODE_BACK 又实测关不掉
			 * 任何顶层窗 ⇒ 长按弹那个含"关闭"的菜单是投影仪**唯一**能触摸关掉它的入口。 */
			/* 补发右键。注意别指望把光标挪到手指处：19:03 实测 Android 上
			 * `QCursor::setPos()` 是空操作（setPos(533,333) 之后 pos() 仍返回上一次 tap 的
			 * 377,279），而菜单定位读的就是 pos() —— 所以修在菜单那一头
			 * （OBSProjector::mousePressEvent 改用 event->globalPosition()）。 */
			QElapsedTimer held3;
			held3.start();
			QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(pressGlobal),
					  Qt::RightButton, Qt::RightButton, Qt::NoModifier);
			QApplication::sendEvent(w, &press); // 到这里阻塞，直到菜单被点掉
			QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), QPointF(pressGlobal),
					    Qt::RightButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(w, &release);
			const long long spent3 = (long long)held3.elapsed();
			obsAndroidDisplayLog("3-4 长按→右键：补发的右键菜单已关 用时=%lldms", spent3);
			return spent3 > 0;
		}
	}

	QTimer timer{this};
	QPointer<QWidget> target;
	QPoint pressGlobal;
	bool armed = false;
	bool backEaten = false;
	QPointer<QComboBox> comboPress; // P-8：这发按下落在组合框上（松手时据此决定吃不吃）
};

#endif // __ANDROID__

OBSApp::OBSApp(int &argc, char **argv, profiler_name_store_t *store)
	: QApplication(argc, argv),
	  profilerNameStore(store),
	  appLaunchUUID_(QUuid::createUuid())
{
	installNativeEventFilter(new OBS::NativeEventFilter);

#ifdef __ANDROID__
	/* 装在 app 上：所有控件的鼠标**和键盘**事件都过这一道（跟着进程活，不用管析构）。
	 * ① 3-4 长按合成右键；② build 74 返回键当 Escape 用。 */
	installEventFilter(new AndroidInputFilter(this));
	AndroidTouchScrollGuard::globalSlot() = new AndroidTouchScrollGuard(this);

	// A Java QtWindow can host multiple QWidget editors. Mark every Qt focus
	// change so the IME bridge never treats a different editor as a duplicate.
	connect(this, &QGuiApplication::focusObjectChanged, this, [](QObject *) {
		QJniObject::callStaticMethod<void>("com/obsproject/studio/ObsImeFocus", "focusChanged", "()V");
	});

#endif

	/* fix float handling */
#if defined(Q_OS_UNIX)
	if (!setlocale(LC_NUMERIC, "C")) {
		blog(LOG_WARNING, "Failed to set LC_NUMERIC to C locale");
	}
#endif

#ifndef _WIN32
	// Add POSIX signal handlers:
	// * SIGINT
	// * SIGTERM
	// * SIGABRT
	// * SIGQUIT

	using SignalCallback = decltype(&OBSApp::processSigInt);

	auto connectSignal = [this](std::array<int, 2> &fileDescriptor, QPointer<QSocketNotifier> &notifier,
				    SignalCallback callback) -> void {
		socketpair(AF_UNIX, SOCK_STREAM, 0, fileDescriptor.data());
		notifier = new QSocketNotifier(fileDescriptor[1], QSocketNotifier::Read, this);
		connect(notifier, &QSocketNotifier::activated, this, callback);
	};

	connectSignal(sigIntFileDescriptor, sigIntNotifier, &OBSApp::processSigInt);
	connectSignal(sigTermFileDescriptor, sigTermNotifier, &OBSApp::processSigTerm);
	connectSignal(sigAbrtFileDescriptor, sigAbrtNotifier, &OBSApp::processSigAbrt);
	connectSignal(sigQuitFileDescriptor, sigQuitNotifier, &OBSApp::processSigQuit);
#endif
	connect(qApp, &QGuiApplication::commitDataRequest, this, &OBSApp::commitData, Qt::DirectConnection);

	if (multi) {
		crashHandler_ = std::make_unique<OBS::CrashHandler>();
	} else {
		crashHandler_ = std::make_unique<OBS::CrashHandler>(appLaunchUUID_);
	}

	sleepInhibitor = os_inhibit_sleep_create("OBS Video/audio");

#ifndef __APPLE__
	setWindowIcon(QIcon::fromTheme("obs", QIcon(":/res/images/obs.png")));
#endif

	setDesktopFileName("com.obsproject.Studio");

	pluginManager_ = std::make_unique<OBS::PluginManager>();
}

OBSApp::~OBSApp()
{
	if (libobs_initialized) {
		applicationShutdown();
	}
};

static void move_basic_to_profiles(void)
{
	char path[512];

	if (GetAppConfigPath(path, 512, "obs-studio/basic") <= 0) {
		return;
	}

	const std::filesystem::path basicPath = std::filesystem::u8path(path);

	if (!std::filesystem::exists(basicPath)) {
		return;
	}

	const std::filesystem::path profilesPath =
		App()->userProfilesLocation / std::filesystem::u8path("obs-studio/basic/profiles");

	if (std::filesystem::exists(profilesPath)) {
		return;
	}

	try {
		std::filesystem::create_directories(profilesPath);
	} catch (const std::filesystem::filesystem_error &error) {
		blog(LOG_ERROR, "Failed to create profiles directory for migration from basic profile\n%s",
		     error.what());
		return;
	}

	const std::filesystem::path newProfilePath = profilesPath / std::filesystem::u8path(Str("Untitled"));

	for (auto &entry : std::filesystem::directory_iterator(basicPath)) {
		if (entry.is_directory()) {
			continue;
		}

		if (entry.path().filename().u8string() == "scenes.json") {
			continue;
		}

		if (!std::filesystem::exists(newProfilePath)) {
			try {
				std::filesystem::create_directory(newProfilePath);
			} catch (const std::filesystem::filesystem_error &error) {
				blog(LOG_ERROR, "Failed to create profile directory for 'Untitled'\n%s", error.what());
				return;
			}
		}

		const filesystem::path destinationFile = newProfilePath / entry.path().filename();

		const auto copyOptions = std::filesystem::copy_options::overwrite_existing;

		try {
			std::filesystem::copy(entry.path(), destinationFile, copyOptions);
		} catch (const std::filesystem::filesystem_error &error) {
			blog(LOG_ERROR, "Failed to copy basic profile file '%s' to new profile 'Untitled'\n%s",
			     entry.path().filename().u8string().c_str(), error.what());

			return;
		}
	}
}

static void move_basic_to_scene_collections(void)
{
	char path[512];

	if (GetAppConfigPath(path, 512, "obs-studio/basic") <= 0) {
		return;
	}

	const std::filesystem::path basicPath = std::filesystem::u8path(path);

	if (!std::filesystem::exists(basicPath)) {
		return;
	}

	const std::filesystem::path sceneCollectionPath =
		App()->userScenesLocation / std::filesystem::u8path("obs-studio/basic/scenes");

	if (std::filesystem::exists(sceneCollectionPath)) {
		return;
	}

	try {
		std::filesystem::create_directories(sceneCollectionPath);
	} catch (const std::filesystem::filesystem_error &error) {
		blog(LOG_ERROR,
		     "Failed to create scene collection directory for migration from basic scene collection\n%s",
		     error.what());
		return;
	}

	const std::filesystem::path sourceFile = basicPath / std::filesystem::u8path("scenes.json");
	const std::filesystem::path destinationFile =
		(sceneCollectionPath / std::filesystem::u8path(Str("Untitled"))).replace_extension(".json");

	try {
		std::filesystem::rename(sourceFile, destinationFile);
	} catch (const std::filesystem::filesystem_error &error) {
		blog(LOG_ERROR, "Failed to rename basic scene collection file:\n%s", error.what());
		return;
	}
}

void OBSApp::AppInit()
{
	ProfileScope("OBSApp::AppInit");

	QAccessible::installFactory(alignmentSelectorFactory);

	if (!MakeUserDirs()) {
		throw "Failed to create required user directories";
	}
	if (!InitGlobalConfig()) {
		throw "Failed to initialize global config";
	}
	if (!InitLocale()) {
		throw "Failed to load locale";
	}
#ifdef __ANDROID__
	obsAndroidKeepAliveSetLocale(locale.c_str());
#endif
	if (!InitTheme()) {
		throw "Failed to load theme";
	}

	config_set_default_string(userConfig, "Basic", "Profile", Str("Untitled"));
	config_set_default_string(userConfig, "Basic", "ProfileDir", Str("Untitled"));
	config_set_default_string(userConfig, "Basic", "SceneCollection", Str("Untitled"));
	config_set_default_string(userConfig, "Basic", "SceneCollectionFile", Str("Untitled"));
	config_set_default_bool(userConfig, "Basic", "ConfigOnNewProfile", true);

	const std::string_view profileName{config_get_string(userConfig, "Basic", "Profile")};

	if (profileName.empty()) {
		config_set_string(userConfig, "Basic", "Profile", Str("Untitled"));
		config_set_string(userConfig, "Basic", "ProfileDir", Str("Untitled"));
	}

	const std::string_view sceneCollectionName{config_get_string(userConfig, "Basic", "SceneCollection")};

	if (sceneCollectionName.empty()) {
		config_set_string(userConfig, "Basic", "SceneCollection", Str("Untitled"));
		config_set_string(userConfig, "Basic", "SceneCollectionFile", Str("Untitled"));
	}

#ifdef _WIN32
	bool disableAudioDucking = config_get_bool(appConfig, "Audio", "DisableAudioDucking");
	if (disableAudioDucking) {
		DisableAudioDucking(true);
	}
#endif

#ifdef __APPLE__
	if (config_get_bool(appConfig, "Video", "DisableOSXVSync")) {
		EnableOSXVSync(false);
	}
#endif

	UpdateHotkeyFocusSetting(false);

	move_basic_to_profiles();
	move_basic_to_scene_collections();

	if (!MakeUserProfileDirs()) {
		throw "Failed to create profile directories";
	}
}

void OBSApp::checkForUncleanShutdown()
{
	bool hasUncleanShutdown = crashHandler_->hasUncleanShutdown();
#ifdef __ANDROID__
	constexpr bool hasNewCrashLog = false;
#else
	bool hasNewCrashLog = crashHandler_->hasNewCrashLog();
#endif

	if (hasUncleanShutdown) {
		UncleanLaunchAction launchAction = handleUncleanShutdown(hasNewCrashLog);

		safe_mode = launchAction.useSafeMode;

#ifndef __ANDROID__
		if (launchAction.sendCrashReport) {
			crashHandler_->uploadLastCrashLog();
		}
#endif
	}
}

const char *OBSApp::GetRenderModule() const
{
#if defined(_WIN32)
	const char *renderer = config_get_string(appConfig, "Video", "Renderer");

	return (astrcmpi(renderer, "Direct3D 11") == 0) ? DL_D3D11 : DL_OPENGL;
#elif defined(__APPLE__) && defined(__aarch64__)
	const char *renderer = config_get_string(appConfig, "Video", "Renderer");

	return (astrcmpi(renderer, "Metal") == 0) ? DL_METAL : DL_OPENGL;
#else
	return DL_OPENGL;
#endif
}

static bool StartupOBS(const char *locale, profiler_name_store_t *store)
{
	char path[512];

	if (GetAppConfigPath(path, sizeof(path), "obs-studio/plugin_config") <= 0) {
		return false;
	}

	return obs_startup(locale, path, store);
}

inline void OBSApp::ResetHotkeyState(bool inFocus)
{
	obs_hotkey_enable_background_press((inFocus && enableHotkeysInFocus) || (!inFocus && enableHotkeysOutOfFocus));
}

void OBSApp::UpdateHotkeyFocusSetting(bool resetState)
{
	enableHotkeysInFocus = true;
	enableHotkeysOutOfFocus = true;

	const char *hotkeyFocusType = config_get_string(userConfig, "General", "HotkeyFocusType");

	if (astrcmpi(hotkeyFocusType, "DisableHotkeysInFocus") == 0) {
		enableHotkeysInFocus = false;
	} else if (astrcmpi(hotkeyFocusType, "DisableHotkeysOutOfFocus") == 0) {
		enableHotkeysOutOfFocus = false;
	}

	if (resetState) {
		ResetHotkeyState(applicationState() == Qt::ApplicationActive);
	}
}

void OBSApp::DisableHotkeys()
{
	enableHotkeysInFocus = false;
	enableHotkeysOutOfFocus = false;
	ResetHotkeyState(applicationState() == Qt::ApplicationActive);
}

void OBSApp::Exec(VoidFunc func)
{
	func();
}

static void ui_task_handler(obs_task_t task, void *param, bool wait)
{
	auto doTask = [=]() {
		/* to get clang-format to behave */
		task(param);
	};
	QMetaObject::invokeMethod(App(), &OBSApp::Exec, wait ? WaitConnection() : Qt::AutoConnection, doTask);
}

bool OBSApp::OBSInit()
{
	ProfileScope("OBSApp::OBSInit");

	qRegisterMetaType<VoidFunc>("VoidFunc");

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
	if (QApplication::platformName() == "xcb") {
		auto native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();

		obs_set_nix_platform_display(native->display());

		obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);

		blog(LOG_INFO, "Using EGL/X11");
	}

#ifdef ENABLE_WAYLAND
	if (QApplication::platformName().contains("wayland")) {
		auto native = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();

		obs_set_nix_platform_display(native->display());

		obs_set_nix_platform(OBS_NIX_PLATFORM_WAYLAND);
		setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

		blog(LOG_INFO, "Platform: Wayland");
	}
#endif

#endif

#ifdef __APPLE__
	setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
#endif

	if (!StartupOBS(locale.c_str(), GetProfilerNameStore())) {
		return false;
	}

	libobs_initialized = true;

	obs_set_ui_task_handler(ui_task_handler);

#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
	bool browserHWAccel = config_get_bool(appConfig, "General", "BrowserHWAccel");

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "BrowserHWAccel", browserHWAccel);
	obs_apply_private_data(settings);

	blog(LOG_INFO, "Current Date/Time: %s", CurrentDateTimeString().c_str());

	blog(LOG_INFO, "Browser Hardware Acceleration: %s", browserHWAccel ? "true" : "false");
#endif
#ifdef _WIN32
	bool hideFromCapture = config_get_bool(userConfig, "BasicWindow", "HideOBSWindowsFromCapture");
	blog(LOG_INFO, "Hide OBS windows from screen capture: %s", hideFromCapture ? "true" : "false");
#endif

	blog(LOG_INFO, "Qt Version: %s (runtime), %s (compiled)", qVersion(), QT_VERSION_STR);
	blog(LOG_INFO, "Portable mode: %s", portable_mode ? "true" : "false");

	if (safe_mode) {
		blog(LOG_WARNING, "Safe Mode enabled.");
	} else if (disable_3p_plugins) {
		blog(LOG_WARNING, "Third-party plugins disabled.");
	}

	setQuitOnLastWindowClosed(false);

	thumbnailManager = new ThumbnailManager(this);

	mainWindow = new OBSBasic();

	mainWindow->setAttribute(Qt::WA_DeleteOnClose, true);

	mainWindow->OBSInit();

	connect(OBSBasic::Get(), &OBSBasic::mainWindowClosed, crashHandler_.get(),
		&OBS::CrashHandler::applicationShutdownHandler);

	connect(this, &QGuiApplication::applicationStateChanged, this,
		[this](Qt::ApplicationState state) { ResetHotkeyState(state == Qt::ApplicationActive); });
	ResetHotkeyState(applicationState() == Qt::ApplicationActive);

#ifndef __ANDROID__
	connect(crashHandler_.get(), &OBS::CrashHandler::crashLogUploadFailed, this,
		[this](const QString &errorMessage) {
			emit this->logUploadFailed(OBS::LogFileType::CrashLog, errorMessage);
		});

	connect(crashHandler_.get(), &OBS::CrashHandler::crashLogUploadFinished, this,
		[this](const QString &fileUrl) { emit this->logUploadFinished(OBS::LogFileType::CrashLog, fileUrl); });
#endif

	return true;
}

string OBSApp::GetVersionString(bool platform) const
{
	stringstream ver;

	ver << obs_get_version_string();

	if (platform) {
		ver << " (";
#ifdef _WIN32
		if (sizeof(void *) == 8) {
			ver << "64-bit, ";
		} else {
			ver << "32-bit, ";
		}

		ver << "windows)";
#elif __APPLE__
		ver << "mac)";
#elif __OpenBSD__
		ver << "openbsd)";
#elif __FreeBSD__
		ver << "freebsd)";
#else /* assume linux for the time being */
		ver << "linux)";
#endif
	}

	return ver.str();
}

bool OBSApp::IsPortableMode()
{
	return portable_mode;
}

bool OBSApp::IsUpdaterDisabled()
{
	return opt_disable_updater;
}

bool OBSApp::IsMissingFilesCheckDisabled()
{
	return opt_disable_missing_files_check;
}

#ifdef __APPLE__
#define INPUT_AUDIO_SOURCE "coreaudio_input_capture"
#define OUTPUT_AUDIO_SOURCE "coreaudio_output_capture"
#elif _WIN32
#define INPUT_AUDIO_SOURCE "wasapi_input_capture"
#define OUTPUT_AUDIO_SOURCE "wasapi_output_capture"
#elif defined(__ANDROID__)
/* android-audio registers 'android_audio_input' (AAudio capture). Without this
 * mapping the UI asked for pulse_input_capture, which does not exist on Android,
 * so no audio input device was ever created. */
#define INPUT_AUDIO_SOURCE "android_audio_input"
#define OUTPUT_AUDIO_SOURCE "pulse_output_capture"
#else
#define INPUT_AUDIO_SOURCE "pulse_input_capture"
#define OUTPUT_AUDIO_SOURCE "pulse_output_capture"
#endif

const char *OBSApp::InputAudioSource() const
{
	return INPUT_AUDIO_SOURCE;
}

const char *OBSApp::OutputAudioSource() const
{
	return OUTPUT_AUDIO_SOURCE;
}

const char *OBSApp::GetLastLog() const
{
	return lastLogFile.c_str();
}

const char *OBSApp::GetCurrentLog() const
{
	return currentLogFile.c_str();
}

void OBSApp::openCrashLogDirectory() const
{
	std::filesystem::path crashLogDirectory = crashHandler_->getCrashLogDirectory();

	if (crashLogDirectory.empty()) {
		return;
	}

	QString crashLogDirectoryString = QString::fromStdString(crashLogDirectory.u8string());

#ifdef __ANDROID__
	obsAndroidShowLocalPath(GetMainWindow(), QTStr("Android.OpenCrashLogFolder"), crashLogDirectoryString);
#else
	QDesktopServices::openUrl(QUrl::fromLocalFile(crashLogDirectoryString));
#endif
}

#ifndef __ANDROID__
void OBSApp::uploadLastAppLog() const
{
	OBSBasic *basicWindow = static_cast<OBSBasic *>(GetMainWindow());

	basicWindow->UploadLog("obs-studio/logs", GetLastLog(), OBS::LogFileType::LastAppLog);
}

void OBSApp::uploadCurrentAppLog() const
{
	OBSBasic *basicWindow = static_cast<OBSBasic *>(GetMainWindow());

	basicWindow->UploadLog("obs-studio/logs", GetCurrentLog(), OBS::LogFileType::CurrentAppLog);
}

void OBSApp::uploadLastCrashLog()
{
	crashHandler_->uploadLastCrashLog();
}

OBS::LogFileState OBSApp::getLogFileState(OBS::LogFileType type) const
{
	switch (type) {
	case OBS::LogFileType::CrashLog: {
		bool hasNewCrashLog = crashHandler_->hasNewCrashLog();

		return (hasNewCrashLog) ? OBS::LogFileState::New : OBS::LogFileState::Uploaded;
	}
	case OBS::LogFileType::CurrentAppLog:
	case OBS::LogFileType::LastAppLog:
		return OBS::LogFileState::New;
	default:
		return OBS::LogFileState::NoState;
	}
}
#endif

bool OBSApp::TranslateString(const char *lookupVal, const char **out) const
{
	for (obs_frontend_translate_ui_cb cb : translatorHooks) {
		if (cb(lookupVal, out)) {
			return true;
		}
	}

	return text_lookup_getstr(App()->GetTextLookup(), lookupVal, out);
}

QStyle *OBSApp::GetInvisibleCursorStyle()
{
	if (!invisibleCursorStyle) {
		invisibleCursorStyle = std::make_unique<OBSInvisibleCursorProxyStyle>();
	}
	return invisibleCursorStyle.get();
}

// Global handler to receive all QEvent::Show events so we can apply
// display affinity on any newly created windows and dialogs without
// caring where they are coming from (e.g. plugins).
bool OBSApp::notify(QObject *receiver, QEvent *e)
{
	QWidget *w;
	QWindow *window;
	int windowType;

#ifdef __ANDROID__
	// QApplication handles this event before normal application event filters.
	// Keep this opt-in probe content-free and observe before Qt handles it.
	if (e->type() == QEvent::RequestSoftwareInputPanel &&
	    qEnvironmentVariable("OBS_ANDROID_IME_DIAGNOSTICS") == QLatin1String("1")) {
		__android_log_print(ANDROID_LOG_INFO, "OBS-IME", "request-panel receiver=%p focusWidget=%p focusObject=%p visible=%d",
				    static_cast<void *>(receiver), static_cast<void *>(QApplication::focusWidget()),
				    static_cast<void *>(QGuiApplication::focusObject()),
				    int(QGuiApplication::inputMethod()->isVisible()));
	}

	bool touchGuardResult = false;
	if (auto *guard = AndroidTouchScrollGuard::current();
	    guard && !QCoreApplication::closingDown() &&
	    guard->beforeNotify(this, receiver, e, touchGuardResult))
		return touchGuardResult;
#endif

	if (!receiver->isWidgetType()) {
		goto skip;
	}

	if (e->type() != QEvent::Show) {
		goto skip;
	}

	w = qobject_cast<QWidget *>(receiver);

	if (!w->isWindow()) {
		goto skip;
	}

	window = w->windowHandle();
	if (!window) {
		goto skip;
	}

	windowType = window->flags() & Qt::WindowType::WindowType_Mask;

	if (windowType == Qt::WindowType::Dialog || windowType == Qt::WindowType::Window ||
	    windowType == Qt::WindowType::Tool) {
		OBSBasic *main = OBSBasic::Get();
		if (main) {
			main->SetDisplayAffinity(window);
		}
	}

skip:
#ifdef __ANDROID__
	const bool notified = QApplication::notify(receiver, e);
	if (auto *guard = AndroidTouchScrollGuard::current(); guard && !QCoreApplication::closingDown())
		guard->afterNotify(e);
	return notified;
#else
	return QApplication::notify(receiver, e);
#endif
}

string GenerateTimeDateFilename(const char *extension, bool noSpace)
{
	time_t now = time(0);
	char file[256] = {};
	struct tm *cur_time;

	cur_time = localtime(&now);
	snprintf(file, sizeof(file), "%d-%02d-%02d%c%02d-%02d-%02d.%s", cur_time->tm_year + 1900, cur_time->tm_mon + 1,
		 cur_time->tm_mday, noSpace ? '_' : ' ', cur_time->tm_hour, cur_time->tm_min, cur_time->tm_sec,
		 extension);

	return string(file);
}

string GenerateSpecifiedFilename(const char *extension, bool noSpace, const char *format)
{
	BPtr<char> filename = os_generate_formatted_filename(extension, !noSpace, format);
	return string(filename);
}

static void FindBestFilename(string &strPath, bool noSpace)
{
	int num = 2;

	if (!os_file_exists(strPath.c_str())) {
		return;
	}

	const char *ext = strrchr(strPath.c_str(), '.');
	if (!ext) {
		return;
	}

	int extStart = int(ext - strPath.c_str());
	for (;;) {
		string testPath = strPath;
		string numStr;

		numStr = noSpace ? "_" : " (";
		numStr += to_string(num++);
		if (!noSpace) {
			numStr += ")";
		}

		testPath.insert(extStart, numStr);

		if (!os_file_exists(testPath.c_str())) {
			strPath = testPath;
			break;
		}
	}
}

static void ensure_directory_exists(string &path)
{
	replace(path.begin(), path.end(), '\\', '/');

	size_t last = path.rfind('/');
	if (last == string::npos) {
		return;
	}

	string directory = path.substr(0, last);
	os_mkdirs(directory.c_str());
}

static void remove_reserved_file_characters(string &s)
{
	replace(s.begin(), s.end(), '\\', '/');
	replace(s.begin(), s.end(), '*', '_');
	replace(s.begin(), s.end(), '?', '_');
	replace(s.begin(), s.end(), '"', '_');
	replace(s.begin(), s.end(), '|', '_');
	replace(s.begin(), s.end(), ':', '_');
	replace(s.begin(), s.end(), '>', '_');
	replace(s.begin(), s.end(), '<', '_');
}

string GetFormatString(const char *format, const char *prefix, const char *suffix)
{
	string f;

	f = format;

	if (prefix && *prefix) {
		string str_prefix = prefix;

		if (str_prefix.back() != ' ') {
			str_prefix += " ";
		}

		size_t insert_pos = 0;
		size_t tmp;

		tmp = f.find_last_of('/');
		if (tmp != string::npos && tmp > insert_pos) {
			insert_pos = tmp + 1;
		}

		tmp = f.find_last_of('\\');
		if (tmp != string::npos && tmp > insert_pos) {
			insert_pos = tmp + 1;
		}

		f.insert(insert_pos, str_prefix);
	}

	if (suffix && *suffix) {
		if (*suffix != ' ') {
			f += " ";
		}
		f += suffix;
	}

	remove_reserved_file_characters(f);

	return f;
}

string GetFormatExt(const char *container)
{
	string ext = container;
	if (ext == "fragmented_mp4" || ext == "hybrid_mp4") {
		ext = "mp4";
	} else if (ext == "fragmented_mov" || ext == "hybrid_mov") {
		ext = "mov";
	} else if (ext == "hls") {
		ext = "m3u8";
	} else if (ext == "mpegts") {
		ext = "ts";
	}

	return ext;
}

string GetOutputFilename(const char *path, const char *container, bool noSpace, bool overwrite, const char *format)
{
	OBSBasic *main = OBSBasic::Get();

#ifdef __ANDROID__
	if (path && obsAndroidStorageManaged(QString::fromUtf8(path))) {
		const auto name = GenerateSpecifiedFilename(GetFormatExt(container).c_str(), noSpace, format);
		const auto uri = obsAndroidStorageCreate(QString::fromUtf8(path), QString::fromStdString(name));
		if (uri.isEmpty()) {
			OBSMessageBox::warning(main, QTStr("Output.BadPath.Title"),
				QStringLiteral("无法在所选文件夹创建录像。请重新选择并授权目录，确认存储空间充足。"
					       "录像文件名不能包含子目录。"));
		}
		// The provider resolves name collisions without overwriting existing recordings.
		return uri.toStdString();
	}
#endif

	os_dir_t *dir = path && path[0] ? os_opendir(path) : nullptr;

	if (!dir) {
		if (main->isVisible()) {
			OBSMessageBox::warning(main, QTStr("Output.BadPath.Title"), QTStr("Output.BadPath.Text"));
		} else {
			main->SysTrayNotify(QTStr("Output.BadPath.Text"), QSystemTrayIcon::Warning);
		}
		return "";
	}

	os_closedir(dir);

	string strPath;
	strPath += path;

	char lastChar = strPath.back();
	if (lastChar != '/' && lastChar != '\\') {
		strPath += "/";
	}

	string ext = GetFormatExt(container);
	strPath += GenerateSpecifiedFilename(ext.c_str(), noSpace, format);
	ensure_directory_exists(strPath);
	if (!overwrite) {
		FindBestFilename(strPath, noSpace);
	}

	return strPath;
}

vector<pair<string, string>> GetLocaleNames()
{
	string path;
	if (!GetDataFilePath("locale.ini", path)) {
		throw "Could not find locale.ini path";
	}

	ConfigFile ini;
	if (ini.Open(path.c_str(), CONFIG_OPEN_EXISTING) != 0) {
		throw "Could not open locale.ini";
	}

	size_t sections = config_num_sections(ini);

	vector<pair<string, string>> names;
	names.reserve(sections);
	for (size_t i = 0; i < sections; i++) {
		const char *tag = config_get_section(ini, i);
		const char *name = config_get_string(ini, tag, "Name");
		names.emplace_back(tag, name);
	}

	return names;
}

#if defined(__APPLE__) || defined(__linux__)
#define BASE_PATH ".."
#else
#define BASE_PATH "../.."
#endif

#define CONFIG_PATH BASE_PATH "/config"

#if defined(ENABLE_PORTABLE_CONFIG) || defined(_WIN32)
#define ALLOW_PORTABLE_MODE 1
#else
#define ALLOW_PORTABLE_MODE 0
#endif

int GetAppConfigPath(char *path, size_t size, const char *name)
{
#if ALLOW_PORTABLE_MODE
	if (portable_mode) {
		if (name && *name) {
			return snprintf(path, size, CONFIG_PATH "/%s", name);
		} else {
			return snprintf(path, size, CONFIG_PATH);
		}
	} else {
		return os_get_config_path(path, size, name);
	}
#else
	return os_get_config_path(path, size, name);
#endif
}

char *GetAppConfigPathPtr(const char *name)
{
#if ALLOW_PORTABLE_MODE
	if (portable_mode) {
		char path[512];

		if (snprintf(path, sizeof(path), CONFIG_PATH "/%s", name) > 0) {
			return bstrdup(path);
		} else {
			return NULL;
		}
	} else {
		return os_get_config_path_ptr(name);
	}
#else
	return os_get_config_path_ptr(name);
#endif
}

int GetProgramDataPath(char *path, size_t size, const char *name)
{
	return os_get_program_data_path(path, size, name);
}

char *GetProgramDataPathPtr(const char *name)
{
	return os_get_program_data_path_ptr(name);
}

bool GetFileSafeName(const char *name, std::string &file)
{
	size_t base_len = strlen(name);
	size_t len = os_utf8_to_wcs(name, base_len, nullptr, 0);
	std::wstring wfile;

	if (!len) {
		return false;
	}

	wfile.resize(len);
	os_utf8_to_wcs(name, base_len, &wfile[0], len + 1);

	for (size_t i = wfile.size(); i > 0; i--) {
		size_t im1 = i - 1;

		if (iswspace(wfile[im1])) {
			wfile[im1] = '_';
		} else if (wfile[im1] != '_' && !iswalnum(wfile[im1])) {
			wfile.erase(im1, 1);
		}
	}

	if (wfile.size() == 0) {
		wfile = L"characters_only";
	}

	len = os_wcs_to_utf8(wfile.c_str(), wfile.size(), nullptr, 0);
	if (!len) {
		return false;
	}

	file.resize(len);
	os_wcs_to_utf8(wfile.c_str(), wfile.size(), &file[0], len + 1);
	return true;
}

bool GetClosestUnusedFileName(std::string &path, const char *extension)
{
	size_t len = path.size();
	if (extension) {
		path += ".";
		path += extension;
	}

	if (!os_file_exists(path.c_str())) {
		return true;
	}

	int index = 1;

	do {
		path.resize(len);
		path += std::to_string(++index);
		if (extension) {
			path += ".";
			path += extension;
		}
	} while (os_file_exists(path.c_str()));

	return true;
}

bool WindowPositionValid(QRect rect)
{
	for (QScreen *screen : QGuiApplication::screens()) {
		if (screen->availableGeometry().intersects(rect)) {
			return true;
		}
	}
	return false;
}

#ifndef _WIN32
// Static signal handlers
void OBSApp::sigIntSignalHandler(int)
{
	char tmp = 1;
	::send(sigIntFileDescriptor[0], &tmp, sizeof(tmp), 0);
}

void OBSApp::sigTermSignalHandler(int)
{
	char tmp = 1;
	::send(sigTermFileDescriptor[0], &tmp, sizeof(tmp), 0);
}

void OBSApp::sigAbrtSignalHandler(int)
{
	char tmp = 1;
	::send(sigAbrtFileDescriptor[0], &tmp, sizeof(tmp), 0);
}

void OBSApp::sigQuitSignalHandler(int)
{
	char tmp = 1;
	::send(sigQuitFileDescriptor[0], &tmp, sizeof(tmp), 0);
}

// App instance signal processors
void OBSApp::processSigInt()
{
	if (!sigIntNotifier->isEnabled()) {
		return;
	}

	sigIntNotifier->setEnabled(false);

	char tmp;
	::recv(sigIntFileDescriptor[1], &tmp, sizeof(tmp), 0);

	sigIntNotifier->setEnabled(true);

#ifndef __APPLE__
	OBSBasic *main = OBSBasic::Get();
	if (main) {
		main->saveAll();
		main->close();
	}
#else
	quit();
#endif
}

void OBSApp::processSigTerm()
{
	if (!sigTermNotifier->isEnabled()) {
		return;
	}

	sigTermNotifier->setEnabled(false);

	char tmp;
	::recv(sigTermFileDescriptor[1], &tmp, sizeof(tmp), 0);

	sigTermNotifier->setEnabled(true);

#ifndef __APPLE__
	OBSBasic *main = OBSBasic::Get();
	if (main) {
		main->saveAll();
	}
#endif
	quit();
}

void OBSApp::processSigAbrt()
{
	if (!sigAbrtNotifier->isEnabled()) {
		return;
	}

	sigAbrtNotifier->setEnabled(false);

	char tmp;
	::recv(sigAbrtFileDescriptor[1], &tmp, sizeof(tmp), 0);

	sigAbrtNotifier->setEnabled(true);

#ifndef __APPLE__
	OBSBasic *main = OBSBasic::Get();
	if (main) {
		main->saveAll();
	}
#endif
	quit();
}

void OBSApp::processSigQuit()
{
	if (!sigQuitNotifier->isEnabled()) {
		return;
	}

	sigQuitNotifier->setEnabled(false);

	char tmp;
	::recv(sigQuitFileDescriptor[1], &tmp, sizeof(tmp), 0);

	sigQuitNotifier->setEnabled(true);

#ifndef __APPLE__
	OBSBasic *main = OBSBasic::Get();
	if (main) {
		main->saveAll();
	}
#endif
	quit();
}
#else
// App instance signal processor stub methods used on Windows for OBSApp API compliance
void OBSApp::processSigInt()
{
	return;
}

void OBSApp::processSigTerm()
{
	return;
}

void OBSApp::processSigAbrt()
{
	return;
}

void OBSApp::processSigQuit()
{
	return;
}
#endif

void OBSApp::commitData(QSessionManager &manager)
{
	OBSBasic *main = OBSBasic::Get();
	if (main) {
		main->saveAll();

		if (manager.allowsInteraction() && main->shouldPromptForClose()) {
			manager.cancel();
		}
	}
}

void OBSApp::applicationShutdown() noexcept
{
#ifdef _WIN32
	bool disableAudioDucking = config_get_bool(appConfig, "Audio", "DisableAudioDucking");
	if (disableAudioDucking) {
		DisableAudioDucking(false);
	}
#else
	auto disconnectSignal = [this](std::array<int, 2> &fileDescriptor,
				       QPointer<QSocketNotifier> &notifier) -> void {
		notifier->setEnabled(false);

		std::array<int, 2> tempFileDescriptor = std::exchange(fileDescriptor, {0, 0});
		::close(tempFileDescriptor[0]);
		::close(tempFileDescriptor[1]);
	};

	disconnectSignal(sigIntFileDescriptor, sigIntNotifier);
	disconnectSignal(sigTermFileDescriptor, sigTermNotifier);
	disconnectSignal(sigAbrtFileDescriptor, sigAbrtNotifier);
	disconnectSignal(sigQuitFileDescriptor, sigQuitNotifier);
#endif

#ifdef __APPLE__
	bool vsyncDisabled = config_get_bool(appConfig, "Video", "DisableOSXVSync");
	bool resetVSync = config_get_bool(appConfig, "Video", "ResetOSXVSyncOnExit");
	if (vsyncDisabled && resetVSync) {
		EnableOSXVSync(true);
	}
#endif

	os_inhibit_sleep_set_active(sleepInhibitor, false);
	os_inhibit_sleep_destroy(sleepInhibitor);

	if (libobs_initialized) {
		obs_shutdown();
#ifdef __ANDROID__
		OBSAndroidBrowserShutdown();
		OBSAndroidAudioShutdown();
#endif
		libobs_initialized = false;
	}
}

void OBSApp::addLogLine(int logLevel, const QString &message)
{
	emit logLineAdded(logLevel, message);
}

void OBSApp::loadAppModules()
{
#ifdef __ANDROID__
	if (!EnsureAndroidPluginBinaries())
		throw "Could not prepare the Android plugin bundle. Check the app log and restart OBS.";
#endif

	using PluginMode = OBS::PluginManager::Mode;
	PluginMode mode = (disable_3p_plugins || safe_mode) ? PluginMode::CoreOnly : PluginMode::Full;
	pluginManager_->setPluginMode(mode);

	pluginManager_->loadAllPlugins(portable_mode);
}

void OBSApp::handlePluginLoadState()
{
	using PluginState = OBS::PluginManager::State;
	PluginState loadState = pluginManager_->loadState();

	if (loadState != PluginState::Success) {
		PluginFailureAction action = handlePluginFailure();

		if (action == PluginFailureAction::OpenPluginManager) {
			pluginManagerOpenDialog();
		}
	}
}

void OBSApp::pluginManagerOpenDialog()
{
	pluginManager_->open();
}
