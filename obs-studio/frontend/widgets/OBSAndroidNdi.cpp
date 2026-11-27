/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "OBSAndroidNdi.hpp"

#ifdef __ANDROID__

#include <OBSApp.hpp>
#include <obs.h>
#include <util/config-file.h>

#include <QAction>
#include <QByteArray>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJniObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <utility/AndroidJniContext.hpp>

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

static const char kNdiHostClass[] = "com/obsproject/studio/ObsNdiHost";
static const char kNdiInstallerClass[] = "com/obsproject/studio/ObsNdiRuntimeInstaller";

static void ndiCopyField(char *dst, size_t dstLen, const QString &src)
{
	if (!dst || dstLen == 0)
		return;
	const QByteArray utf8 = src.toUtf8();
	const size_t cap = dstLen - 1;
	const size_t n = (size_t)utf8.size() < cap ? (size_t)utf8.size() : cap;
	memcpy(dst, utf8.constData(), n);
	dst[n] = '\0';
}

static bool jbNdiAcquireDiscovery(void *param)
{
	(void)param;
	return QJniObject::callStaticMethod<jboolean>(kNdiHostClass, "acquireDiscovery", "()Z") != JNI_FALSE;
}

static void jbNdiReleaseDiscovery(void *param)
{
	(void)param;
	QJniObject::callStaticMethod<void>(kNdiHostClass, "releaseDiscovery", "()V");
}

static int jbNdiGetPeers(void *param, struct obs_android_ndi_peer *out, int max)
{
	(void)param;
	const QJniObject snap =
		QJniObject::callStaticObjectMethod(kNdiHostClass, "snapshotPeers", "()Ljava/lang/String;");
	if (!snap.isValid())
		return -1;
	const QString joined = snap.toString();
	if (joined.isEmpty())
		return 0;

	const QStringList entries = joined.split(QLatin1Char(';'), Qt::SkipEmptyParts);
	int n = 0;
	for (const QString &e : entries) {
		if (n >= max)
			break;
		const QStringList f = e.split(QLatin1Char('|'));
		if (f.size() < 2)
			continue;
		ndiCopyField(out[n].name, sizeof(out[n].name), f.at(0));
		ndiCopyField(out[n].address, sizeof(out[n].address), f.at(1));
		n++;
	}
	return n;
}

static bool jbNdiGetNativeLibDir(void *param, char *out, size_t size)
{
	(void)param;
	if (!out || size == 0)
		return false;
	const QJniObject dir =
		QJniObject::callStaticObjectMethod(kNdiHostClass, "nativeLibDir", "()Ljava/lang/String;");
	if (!dir.isValid())
		return false;
	const QString s = dir.toString();
	if (s.isEmpty())
		return false;
	ndiCopyField(out, size, s);
	return out[0] != '\0';
}

static const struct obs_android_ndi_host kNdiHost = {
	nullptr,
	jbNdiAcquireDiscovery,
	jbNdiReleaseDiscovery,
	jbNdiGetPeers,
	jbNdiGetNativeLibDir,
};

bool obsAndroidNdiRegisterBus(QString *detail)
{
	static bool s_registered = false;
	if (s_registered) {
		if (detail)
			*detail = QStringLiteral("NDI 发现桥已注册");
		return true;
	}
	s_registered = true;

	const QJniObject ctx = obsAndroidAppContext();
	if (!ctx.isValid()) {
		const QString line = QStringLiteral("NDI 宿主桥初始化失败：无法获取 Android Context");
		if (detail)
			*detail = line;
		blog(LOG_WARNING, "OBSAndroidNdi[Android]: %s", line.toUtf8().constData());
		return false;
	}

	const jboolean ok = QJniObject::callStaticMethod<jboolean>(kNdiHostClass, "install",
								   "(Landroid/content/Context;)Z", ctx.object<jobject>());
	if (!ok) {
		const QString line = QStringLiteral("NDI 宿主桥初始化失败：ObsNdiHost.install() 返回 false");
		if (detail)
			*detail = line;
		blog(LOG_WARNING, "OBSAndroidNdi[Android]: %s", line.toUtf8().constData());
		return false;
	}

	obs_android_set_ndi_host(&kNdiHost);
	char libdir[256] = {0};
	obs_android_ndi_native_lib_dir(libdir, sizeof(libdir));
	const QString line = QStringLiteral("NDI 发现桥已注册，ready=%1，nativeLibDir=%2")
				     .arg(obs_android_ndi_ready() ? 1 : 0)
				     .arg(QString::fromUtf8(libdir));
	if (detail)
		*detail = line;
	blog(LOG_INFO, "OBSAndroidNdi: %s", line.toUtf8().constData());
	return true;
}

static obs_output_t *g_ndiMainOutput = nullptr;
static bool g_ndiMainEnabled = false;

static void stopNdiMainOutput()
{
	if (!g_ndiMainOutput)
		return;
	if (obs_output_active(g_ndiMainOutput))
		obs_output_stop(g_ndiMainOutput);
	obs_output_release(g_ndiMainOutput);
	g_ndiMainOutput = nullptr;
	g_ndiMainEnabled = false;
}

static bool applyNdiMainOutput(bool enable, const QString &name, const QString &groups, QString *errOut)
{
	stopNdiMainOutput();
	if (!enable)
		return true;

	obs_data_t *settings = obs_data_create();
	const QString cleanName = name.trimmed().isEmpty() ? QStringLiteral("OBS for Android") : name.trimmed();
	obs_data_set_string(settings, "ndi_name", cleanName.toUtf8().constData());
	obs_data_set_string(settings, "ndi_groups", groups.trimmed().toUtf8().constData());

	obs_output_t *out = obs_output_create("android_ndi_output", "android_ndi_main_output", settings, nullptr);
	obs_data_release(settings);
	if (!out) {
		if (errOut)
			*errOut = QStringLiteral("未找到 android_ndi_output 输出插件，请确认 android-ndi.so 已加载。");
		return false;
	}

	if (!obs_output_start(out)) {
		obs_output_release(out);
		if (errOut)
			*errOut = QStringLiteral("无法启动 NDI® 主程序输出，请确认官方 libndi.so 已正确部署。");
		return false;
	}

	g_ndiMainOutput = out;
	g_ndiMainEnabled = true;
	return true;
}

static QString ndiUserRuntimeDir()
{
	const char *root = std::getenv("OBS_ROOT_PATH");
	if (root && root[0])
		return QString::fromUtf8(root) + QStringLiteral("/ndi");
	return QStringLiteral("/data/user/0/com.obsproject.studio/files/obsroot/ndi");
}

static bool ndiIsSupportedVersion(const char *version)
{
	if (!version)
		return false;
	const char *p = version;
	while (*p) {
		if (*p >= '0' && *p <= '9') {
			char *end = nullptr;
			const long major = std::strtol(p, &end, 10);
			if (end != p && (*end == '.' || *end == '\0'))
				return major == 5 || major == 6;
			p = end;
		} else {
			p++;
		}
	}
	return false;
}

static QString probeNdiRuntimeVersion(const QString &soPath, QString *errOut = nullptr)
{
	if (!QFileInfo::exists(soPath)) {
		if (errOut)
			*errOut = QStringLiteral("文件不存在");
		return QString();
	}
	void *h = dlopen(soPath.toUtf8().constData(), RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		const char *de = dlerror();
		if (errOut)
			*errOut = QStringLiteral("dlopen 加载失败：%1").arg(de ? QString::fromUtf8(de) : QStringLiteral("unknown"));
		return QString();
	}
	static const char *const kRequiredSyms[] = {
		"NDIlib_initialize",
		"NDIlib_destroy",
		"NDIlib_version",
		"NDIlib_find_create_v2",
		"NDIlib_find_destroy",
		"NDIlib_find_get_current_sources",
		"NDIlib_find_wait_for_sources",
		"NDIlib_recv_create_v3",
		"NDIlib_recv_destroy",
		"NDIlib_recv_connect",
		"NDIlib_recv_capture_v3",
		"NDIlib_recv_free_video_v2",
		"NDIlib_recv_free_audio_v3",
		"NDIlib_recv_free_metadata",
	};
	for (const char *sym : kRequiredSyms) {
		if (!dlsym(h, sym)) {
			if (errOut)
				*errOut = QStringLiteral("缺少必需的 NDI 符号：%1（不是有效的官方 libndi.so）")
						  .arg(QString::fromUtf8(sym));
			dlclose(h);
			return QString();
		}
	}
	using version_fn = const char *(*)();
	auto fn = reinterpret_cast<version_fn>(dlsym(h, "NDIlib_version"));
	const char *rawVer = fn ? fn() : nullptr;
	if (!ndiIsSupportedVersion(rawVer)) {
		if (errOut)
			*errOut = QStringLiteral("不支持的 NDI 运行库版本：%1（仅支持 NDI 5 / 6）")
					  .arg(rawVer ? QString::fromUtf8(rawVer) : QStringLiteral("unknown"));
		dlclose(h);
		return QString();
	}
	QString ver = QString::fromUtf8(rawVer);
	dlclose(h);
	return ver;
}

static void openNdiSdkDownloadPage()
{
	const QString url = QStringLiteral("https://downloads.ndi.tv/SDK/NDI_SDK_Android/Install_NDI_SDK_v6_Android.tar.gz");
	const QJniObject activity = obsAndroidActivity();
	if (activity.isValid()) {
		const QJniObject urlObj = QJniObject::fromString(url);
		if (QJniObject::callStaticMethod<jboolean>(
			    "com/obsproject/studio/ObsUpdateHost", "openUrl",
			    "(Landroid/content/Context;Ljava/lang/String;)Z", activity.object(),
			    urlObj.object<jstring>()) == JNI_TRUE)
			return;
	}
	QDesktopServices::openUrl(QUrl(url));
}

static void showNdiOutputSettingsDialog(QWidget *parent)
{
	config_t *cfg = App()->GetUserConfig();
	const bool curEnabled = g_ndiMainEnabled ||
				(cfg && config_get_bool(cfg, "AndroidNdiOutput", "EnableMainOutput"));
	const char *savedName = cfg ? config_get_string(cfg, "AndroidNdiOutput", "MainOutputName") : nullptr;
	const char *savedGroups = cfg ? config_get_string(cfg, "AndroidNdiOutput", "MainOutputGroups") : nullptr;

	QDialog dlg(parent);
	dlg.setWindowTitle(QStringLiteral("NDI® 设置"));
	dlg.setMinimumWidth(480);

	auto *layout = new QVBoxLayout(&dlg);

	/* --- Section 1: BYOR Official NDI Runtime Management --- */
	auto *rtGroup = new QGroupBox(QStringLiteral("官方 NDI® 运行库（BYOR 用户自备）"), &dlg);
	auto *rtLayout = new QVBoxLayout(rtGroup);
	auto *rtStatus = new QLabel(&dlg);
	rtStatus->setWordWrap(true);
	rtLayout->addWidget(rtStatus);

	auto *rtBtns = new QHBoxLayout();
	auto *importBtn = new QPushButton(QStringLiteral("导入官方 NDI SDK / libndi.so…"), rtGroup);
	auto *downloadBtn = new QPushButton(QStringLiteral("下载官方 SDK"), rtGroup);
	auto *removeBtn = new QPushButton(QStringLiteral("移除运行库"), rtGroup);
	rtBtns->addWidget(importBtn);
	rtBtns->addWidget(downloadBtn);
	rtBtns->addWidget(removeBtn);
	rtLayout->addLayout(rtBtns);
	layout->addWidget(rtGroup);

	const QString userDir = ndiUserRuntimeDir();
	const QString userSo = userDir + QStringLiteral("/libndi.so");

	auto refreshRuntimeStatus = [rtStatus, removeBtn, userSo]() {
		const bool hasUser = QFileInfo::exists(userSo);
		removeBtn->setEnabled(hasUser);
		if (hasUser) {
			const QString ver = probeNdiRuntimeVersion(userSo);
			if (!ver.isEmpty()) {
				rtStatus->setText(QStringLiteral("状态：已就绪（用户导入 · %1 · 已启用 MediaCodec NDI|HX 硬件解码）").arg(ver));
			} else {
				rtStatus->setText(QStringLiteral("状态：已发现用户导入的 libndi.so，但加载校验失败，请重新导入官方 arm64-v8a 版本。"));
			}
			return;
		}
		char natDir[256] = {0};
		if (obs_android_ndi_native_lib_dir(natDir, sizeof(natDir))) {
			const QString bundledSo = QString::fromUtf8(natDir) + QStringLiteral("/libndi.so");
			const QString ver = probeNdiRuntimeVersion(bundledSo);
			if (!ver.isEmpty()) {
				rtStatus->setText(QStringLiteral("状态：已就绪（本地验证内置 · %1 · 已启用 MediaCodec NDI|HX 硬件解码）").arg(ver));
				return;
			}
		}
		rtStatus->setText(QStringLiteral(
			"状态：未安装官方 NDI® 运行库。为遵循 GPLv2 与 NDI SDK 许可协议，公开版 APK 不内置闭源 libndi.so；"
			"请点击下方「下载官方 SDK」通过浏览器下载官方 .tar.gz（无需手动解压），再点击「导入」选择该文件。"));
	};
	refreshRuntimeStatus();

	QObject::connect(downloadBtn, &QPushButton::clicked, &dlg, [] { openNdiSdkDownloadPage(); });

	QObject::connect(importBtn, &QPushButton::clicked, &dlg, [&dlg, userDir, refreshRuntimeStatus]() {
		const QUrl pickedUrl = QFileDialog::getOpenFileUrl(
			&dlg, QStringLiteral("选择官方 NDI SDK（.tar.gz / .sh）或 arm64-v8a libndi.so"), QUrl(),
			QStringLiteral("NDI SDK / Runtime (*.*)"));
		if (pickedUrl.isEmpty())
			return;
		const QString picked =
			pickedUrl.isLocalFile() ? pickedUrl.toLocalFile() : pickedUrl.toString(QUrl::FullyEncoded);
		if (picked.isEmpty())
			return;

		const QJniObject srcObj = QJniObject::fromString(picked);
		const QJniObject dstObj = QJniObject::fromString(userDir);
		const QJniObject resObj = QJniObject::callStaticObjectMethod(
			kNdiInstallerClass, "importRuntimeToStaging",
			"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
			srcObj.object<jstring>(), dstObj.object<jstring>());
		const QString res = resObj.isValid() ? resObj.toString() : QStringLiteral("ERR:JNI call failed");
		if (!res.startsWith(QLatin1String("OK:"))) {
			QMessageBox::warning(&dlg, QStringLiteral("导入失败"),
					     QStringLiteral("无法导入 NDI® 运行库（已有运行库未被改动）：\n%1").arg(res.mid(4)));
			return;
		}
		const QStringList parts = res.mid(3).split(QLatin1Char('|'));
		const QString stageSo = parts.isEmpty() ? QString() : parts.at(0);
		QString probeErr;
		const QString ver = probeNdiRuntimeVersion(stageSo, &probeErr);
		if (ver.isEmpty()) {
			QJniObject::callStaticMethod<void>(kNdiInstallerClass, "discardRuntimeStaging",
							   "(Ljava/lang/String;)V", dstObj.object<jstring>());
			QMessageBox::warning(&dlg, QStringLiteral("校验失败"),
					     QStringLiteral("所选文件未通过 NDI 运行库校验，已保留原有运行库：\n%1").arg(probeErr));
			return;
		}
		const jboolean committed = QJniObject::callStaticMethod<jboolean>(
			kNdiInstallerClass, "commitRuntimeStaging", "(Ljava/lang/String;)Z", dstObj.object<jstring>());
		refreshRuntimeStatus();
		if (committed == JNI_FALSE) {
			QMessageBox::warning(&dlg, QStringLiteral("导入失败"),
					     QStringLiteral("校验通过但替换运行库文件失败。"));
			return;
		}
		QMessageBox::information(
			&dlg, QStringLiteral("导入成功"),
			QStringLiteral("已成功校验（ARM64 · 16 KB 页对齐 · 全部必需符号）并导入官方 NDI® 运行库：\n%1")
				.arg(ver));
	});

	QObject::connect(removeBtn, &QPushButton::clicked, &dlg, [&dlg, userDir, refreshRuntimeStatus]() {
		stopNdiMainOutput();
		const QJniObject dstObj = QJniObject::fromString(userDir);
		QJniObject::callStaticMethod<jboolean>(kNdiInstallerClass, "removeRuntime", "(Ljava/lang/String;)Z",
						       dstObj.object<jstring>());
		refreshRuntimeStatus();
	});

	/* --- Section 2: NDI Program Output --- */
	auto *outGroup = new QGroupBox(QStringLiteral("NDI® 主程序输出（Program Output）"), &dlg);
	auto *outLayout = new QVBoxLayout(outGroup);

	auto *enableBox = new QCheckBox(QStringLiteral("启用主程序 NDI® 输出（Program Output）"), outGroup);
	enableBox->setChecked(curEnabled);
	outLayout->addWidget(enableBox);

	auto *form = new QFormLayout();
	auto *nameEdit = new QLineEdit(
		(savedName && savedName[0]) ? QString::fromUtf8(savedName) : QStringLiteral("OBS for Android"),
		outGroup);
	auto *groupsEdit = new QLineEdit(savedGroups ? QString::fromUtf8(savedGroups) : QString(), outGroup);
	groupsEdit->setPlaceholderText(QStringLiteral("留空使用默认分组"));
	form->addRow(QStringLiteral("NDI 输出名称"), nameEdit);
	form->addRow(QStringLiteral("NDI 分组（可选）"), groupsEdit);
	outLayout->addLayout(form);

	QString statusText = QStringLiteral("当前输出状态：未启动");
	if (g_ndiMainOutput && obs_output_active(g_ndiMainOutput)) {
		const uint64_t bytes = obs_output_get_total_bytes(g_ndiMainOutput);
		statusText = QStringLiteral("当前输出状态：运行中（已发送 %1 MB）")
				     .arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
	}
	auto *statusLabel = new QLabel(statusText, outGroup);
	statusLabel->setWordWrap(true);
	outLayout->addWidget(statusLabel);
	layout->addWidget(outGroup);

	/* --- Section 3: Trademark & License Attribution --- */
	auto *tmLabel = new QLabel(
		QStringLiteral("NDI® is a registered trademark of Vizrt NDI AB. 本应用与 Vizrt 无隶属或赞助关系；"
			       "官方 NDI 运行库由用户自行获取并受 NDI SDK 许可协议约束。"),
		&dlg);
	tmLabel->setWordWrap(true);
	layout->addWidget(tmLabel);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	layout->addWidget(buttons);

	if (dlg.exec() != QDialog::Accepted)
		return;

	const bool wantEnable = enableBox->isChecked();
	const QString wantName = nameEdit->text().trimmed().isEmpty() ? QStringLiteral("OBS for Android")
								      : nameEdit->text().trimmed();
	const QString wantGroups = groupsEdit->text().trimmed();

	QString err;
	if (!applyNdiMainOutput(wantEnable, wantName, wantGroups, &err)) {
		QMessageBox::warning(parent, QStringLiteral("NDI® 设置"), err);
		if (cfg) {
			config_set_bool(cfg, "AndroidNdiOutput", "EnableMainOutput", false);
			config_save_safe(cfg, "tmp", nullptr);
		}
		return;
	}

	if (cfg) {
		config_set_bool(cfg, "AndroidNdiOutput", "EnableMainOutput", wantEnable);
		config_set_string(cfg, "AndroidNdiOutput", "MainOutputName", wantName.toUtf8().constData());
		config_set_string(cfg, "AndroidNdiOutput", "MainOutputGroups", wantGroups.toUtf8().constData());
		config_save_safe(cfg, "tmp", nullptr);
	}
}

void obsAndroidNdiInitOutputMenu(QWidget *parent, QMenu *toolsMenu)
{
	if (!toolsMenu)
		return;
	QAction *act = toolsMenu->addAction(QStringLiteral("NDI® 设置…"));
	QObject::connect(act, &QAction::triggered, parent, [parent] { showNdiOutputSettingsDialog(parent); });

	/* Restore NDI Program Output after plugins finish loading if previously enabled. */
	QTimer::singleShot(1500, parent, [] {
		config_t *cfg = App()->GetUserConfig();
		if (!cfg || !config_get_bool(cfg, "AndroidNdiOutput", "EnableMainOutput"))
			return;
		const char *savedName = config_get_string(cfg, "AndroidNdiOutput", "MainOutputName");
		const char *savedGroups = config_get_string(cfg, "AndroidNdiOutput", "MainOutputGroups");
		QString err;
		if (!applyNdiMainOutput(true,
					(savedName && savedName[0]) ? QString::fromUtf8(savedName)
								    : QStringLiteral("OBS for Android"),
					savedGroups ? QString::fromUtf8(savedGroups) : QString(), &err)) {
			blog(LOG_WARNING, "OBSAndroidNdi: auto-start NDI output failed: %s",
			     err.toUtf8().constData());
		}
	});
}

void obsAndroidNdiShutdownOutput()
{
	stopNdiMainOutput();
}

#endif
