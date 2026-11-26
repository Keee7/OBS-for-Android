/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

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

#include "OBSBasic.hpp"

#include <dialogs/OBSWhatsNew.hpp>

#ifdef _WIN32
#include <utility/AutoUpdateThread.hpp>
#endif
#ifdef ENABLE_SPARKLE_UPDATER
#include <utility/MacUpdateThread.hpp>
#include <utility/OBSSparkle.hpp>
#endif
#if defined(_WIN32) || defined(WHATSNEW_ENABLED)
#include <utility/WhatsNewBrowserInitThread.hpp>
#include <utility/models/whatsnew.hpp>
#endif

#ifdef BROWSER_AVAILABLE
#include <browser-panel.hpp>
#endif
#include <qt-wrappers.hpp>

#ifdef __ANDROID__
#include <utility/android-window-fit.hpp>

#include <QDesktopServices>
#include <QDialog>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJniObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>
#endif

#ifdef _WIN32
#define UPDATE_CHECK_INTERVAL (60 * 60 * 24 * 4) /* 4 days */
#endif

struct QCef;

#ifdef __ANDROID__
namespace {

constexpr auto ANDROID_GITHUB_REPO = "Keee7/OBS-for-Android";
constexpr auto ANDROID_DEFAULT_RELEASE_URL = "https://github.com/Keee7/OBS-for-Android/releases/latest";

std::atomic<bool> g_androidUpdateInFlight{false};
std::atomic<bool> g_androidShutdownRequested{false};

struct ParsedSemVer {
	std::vector<int> core;
	QString preRelease;
	bool valid = false;
};

ParsedSemVer ParseReleaseVersion(const QString &raw)
{
	ParsedSemVer out;
	QString trimmed = raw.trimmed();
	if (trimmed.isEmpty())
		return out;

	if ((trimmed.startsWith('v') || trimmed.startsWith('V')) && trimmed.size() > 1)
		trimmed = trimmed.mid(1).trimmed();

	const qsizetype plusIdx = trimmed.indexOf('+');
	if (plusIdx >= 0)
		trimmed = trimmed.left(plusIdx).trimmed();

	const qsizetype dashIdx = trimmed.indexOf('-');
	QString corePart = dashIdx >= 0 ? trimmed.left(dashIdx) : trimmed;
	out.preRelease = dashIdx >= 0 ? trimmed.mid(dashIdx + 1).trimmed() : QString();

	const QStringList segments = corePart.split('.', Qt::SkipEmptyParts);
	if (segments.isEmpty())
		return out;

	for (const QString &seg : segments) {
		bool ok = false;
		const int val = seg.toInt(&ok);
		if (!ok || val < 0)
			return {};
		out.core.push_back(val);
	}

	out.valid = true;
	return out;
}

int ComparePreReleaseIds(const QString &a, const QString &b)
{
	const QStringList aParts = a.split('.', Qt::SkipEmptyParts);
	const QStringList bParts = b.split('.', Qt::SkipEmptyParts);
	const qsizetype count = std::min(aParts.size(), bParts.size());

	for (qsizetype i = 0; i < count; ++i) {
		const QString &sa = aParts[i];
		const QString &sb = bParts[i];
		bool aNumOk = false;
		bool bNumOk = false;
		const int na = sa.toInt(&aNumOk);
		const int nb = sb.toInt(&bNumOk);

		if (aNumOk && bNumOk) {
			if (na != nb)
				return na < nb ? -1 : 1;
		} else if (aNumOk != bNumOk) {
			return aNumOk ? -1 : 1;
		} else {
			const int cmp = QString::compare(sa, sb, Qt::CaseInsensitive);
			if (cmp != 0)
				return cmp < 0 ? -1 : 1;
		}
	}

	if (aParts.size() == bParts.size())
		return 0;
	return aParts.size() < bParts.size() ? -1 : 1;
}

int CompareReleaseVersions(const QString &lhsRaw, const QString &rhsRaw)
{
	const ParsedSemVer lhs = ParseReleaseVersion(lhsRaw);
	const ParsedSemVer rhs = ParseReleaseVersion(rhsRaw);

	if (!lhs.valid || !rhs.valid) {
		const int cmp = QString::compare(lhsRaw.trimmed(), rhsRaw.trimmed(), Qt::CaseInsensitive);
		return cmp == 0 ? 0 : (cmp < 0 ? -1 : 1);
	}

	const size_t maxLen = std::max(lhs.core.size(), rhs.core.size());
	for (size_t i = 0; i < maxLen; ++i) {
		const int a = i < lhs.core.size() ? lhs.core[i] : 0;
		const int b = i < rhs.core.size() ? rhs.core[i] : 0;
		if (a != b)
			return a < b ? -1 : 1;
	}

	const bool lhsPre = !lhs.preRelease.isEmpty();
	const bool rhsPre = !rhs.preRelease.isEmpty();
	if (!lhsPre && !rhsPre)
		return 0;
	if (!lhsPre && rhsPre)
		return 1;
	if (lhsPre && !rhsPre)
		return -1;
	return ComparePreReleaseIds(lhs.preRelease, rhs.preRelease);
}

QString GetAndroidInstalledVersionName()
{
	QString version;
	const QJniObject activity = QJniObject::callStaticObjectMethod(
		"org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
	if (activity.isValid()) {
		const QJniObject verObj = QJniObject::callStaticObjectMethod(
			"com/obsproject/studio/ObsUpdateHost", "installedVersionName",
			"(Landroid/content/Context;)Ljava/lang/String;", activity.object());
		if (verObj.isValid())
			version = verObj.toString().trimmed();
	}
	if (version.isEmpty())
		version = QStringLiteral("0.2.9");
	return version;
}

bool OpenAndroidReleaseUrl(const QString &url)
{
	const QString target = url.trimmed().isEmpty() ? QString::fromUtf8(ANDROID_DEFAULT_RELEASE_URL) : url.trimmed();
	const QJniObject activity = QJniObject::callStaticObjectMethod(
		"org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
	if (activity.isValid()) {
		const QJniObject urlObj = QJniObject::fromString(target);
		const jboolean opened = QJniObject::callStaticMethod<jboolean>(
			"com/obsproject/studio/ObsUpdateHost", "openUrl",
			"(Landroid/content/Context;Ljava/lang/String;)Z", activity.object(), urlObj.object<jstring>());
		if (opened == JNI_TRUE)
			return true;
	}
	return QDesktopServices::openUrl(QUrl(target));
}

void ShowAndroidUpdateResultDialog(QWidget *parent, bool manualUpdate, const QString &currentVersion,
				   const QString &rawJson)
{
	static bool dialogOpen = false;
	if (dialogOpen)
		return;

	const QJsonDocument doc = QJsonDocument::fromJson(rawJson.toUtf8());
	const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject{};
	if (!obj.value(QStringLiteral("ok")).toBool(false)) {
		const QString err = obj.value(QStringLiteral("error")).toString().trimmed();
		blog(LOG_INFO, "Android release update check failed (%s): %s",
		     manualUpdate ? "manual" : "startup",
		     err.isEmpty() ? "unknown error" : QT_TO_UTF8(err));
		if (manualUpdate) {
			const QString detail = err.isEmpty() ? QStringLiteral("Network unavailable") : err;
			OBSMessageBox::warning(parent, QTStr("Basic.MainMenu.Help.CheckForUpdates"),
					       QTStr("Updater.CheckFailed").arg(detail));
		}
		return;
	}

	const QString latestVersion = obj.value(QStringLiteral("version")).toString().trimmed();
	const QString latestTag = obj.value(QStringLiteral("tag")).toString().trimmed();
	const QString releaseName = obj.value(QStringLiteral("name")).toString().trimmed();
	const QString releaseUrl = obj.value(QStringLiteral("url")).toString().trimmed();
	const QString releaseNotes = obj.value(QStringLiteral("notes")).toString().trimmed();

	blog(LOG_INFO, "Android release update check (%s): current=%s, latest=%s (%s)",
	     manualUpdate ? "manual" : "startup", QT_TO_UTF8(currentVersion),
	     QT_TO_UTF8(latestVersion), QT_TO_UTF8(latestTag));

	if (latestVersion.isEmpty() || CompareReleaseVersions(latestVersion, currentVersion) <= 0) {
		if (manualUpdate) {
			OBSMessageBox::information(parent, QTStr("Updater.NoUpdatesAvailable.Title"),
						   QTStr("Updater.NoUpdatesAvailable.Text"));
		}
		return;
	}

	if (!manualUpdate) {
		const char *skipped = config_get_string(App()->GetAppConfig(), "General", "SkippedUpdateVersion");
		if (skipped && *skipped &&
		    CompareReleaseVersions(QString::fromUtf8(skipped), latestVersion) >= 0) {
			blog(LOG_INFO, "Skipping Android update prompt for %s (already skipped %s)",
			     QT_TO_UTF8(latestVersion), skipped);
			return;
		}
	}

	dialogOpen = true;
	QDialog dlg(parent);
	dlg.setObjectName(QStringLiteral("OBSAndroidUpdateDialog"));
	dlg.setWindowTitle(QTStr("Updater.Title"));
	dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
	dlg.setModal(true);
	dlg.setMinimumSize(0, 0);

	auto *rootLayout = new QVBoxLayout(&dlg);
	rootLayout->setContentsMargins(16, 14, 16, 14);
	rootLayout->setSpacing(10);

	const QString displayLatest = latestTag.isEmpty() ? latestVersion : latestTag;
	auto *headerLabel = new QLabel(&dlg);
	QFont headerFont = headerLabel->font();
	headerFont.setBold(true);
	headerFont.setPointSizeF(std::max(headerFont.pointSizeF(), 11.0));
	headerLabel->setFont(headerFont);
	headerLabel->setWordWrap(true);
	headerLabel->setText(!releaseName.isEmpty() && releaseName != displayLatest
				     ? QStringLiteral("%1 %2 — %3").arg(QTStr("Updater.Text"), displayLatest, releaseName)
				     : QStringLiteral("%1 %2").arg(QTStr("Updater.Text"), displayLatest));
	rootLayout->addWidget(headerLabel);

	auto *versionLabel = new QLabel(
		QTStr("Updater.VersionSummary").arg(currentVersion, displayLatest), &dlg);
	versionLabel->setWordWrap(true);
	rootLayout->addWidget(versionLabel);

	auto *notesBrowser = new QTextBrowser(&dlg);
	notesBrowser->setReadOnly(true);
	notesBrowser->setOpenLinks(false);
	notesBrowser->setOpenExternalLinks(false);
	notesBrowser->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	QObject::connect(notesBrowser, &QTextBrowser::anchorClicked, &dlg,
			 [](const QUrl &link) { OpenAndroidReleaseUrl(link.toString()); });

	const QString effectiveUrl =
		releaseUrl.isEmpty() ? QString::fromUtf8(ANDROID_DEFAULT_RELEASE_URL) : releaseUrl;
	if (!releaseNotes.isEmpty()) {
		notesBrowser->setMarkdown(releaseNotes);
	} else {
		notesBrowser->setPlainText(QTStr("Updater.ReleasePageHint").arg(effectiveUrl));
	}
	rootLayout->addWidget(notesBrowser, 1);

	auto *buttonLayout = new QHBoxLayout();
	buttonLayout->setSpacing(8);

	constexpr int ACTION_REMIND = QDialog::Rejected;
	constexpr int ACTION_UPDATE = QDialog::Accepted;
	constexpr int ACTION_SKIP = QDialog::Accepted + 1;

	if (!manualUpdate) {
		auto *skipBtn = new QPushButton(QTStr("Updater.Skip"), &dlg);
		skipBtn->setMinimumHeight(34);
		QObject::connect(skipBtn, &QPushButton::clicked, &dlg, [&dlg]() { dlg.done(ACTION_SKIP); });
		buttonLayout->addWidget(skipBtn);
	}

	buttonLayout->addStretch(1);

	auto *laterBtn = new QPushButton(
		QTStr(manualUpdate ? "Cancel" : "Updater.RemindMeLater"), &dlg);
	laterBtn->setMinimumHeight(34);
	QObject::connect(laterBtn, &QPushButton::clicked, &dlg, [&dlg]() { dlg.done(ACTION_REMIND); });
	buttonLayout->addWidget(laterBtn);

	auto *updateBtn = new QPushButton(QTStr("Updater.UpdateNow"), &dlg);
	updateBtn->setMinimumHeight(34);
	updateBtn->setDefault(true);
	QObject::connect(updateBtn, &QPushButton::clicked, &dlg, [&dlg]() { dlg.done(ACTION_UPDATE); });
	buttonLayout->addWidget(updateBtn);

	rootLayout->addLayout(buttonLayout);

	const QRect avail = androidAvailableWindowRect(&dlg);
	if (!avail.isEmpty()) {
		const int w = qBound(280, qMin(560, avail.width() - 24), avail.width());
		const int h = qBound(220, qMin(330, avail.height() - 24), avail.height());
		dlg.resize(w, h);
	} else {
		dlg.resize(520, 320);
	}
	androidFitTopLevelToScreen(&dlg);
	if (!avail.isEmpty()) {
		dlg.move(avail.center() - QPoint(dlg.width() / 2, dlg.height() / 2));
	}

	const int choice = dlg.exec();
	dialogOpen = false;

	if (choice == ACTION_UPDATE) {
		if (!OpenAndroidReleaseUrl(effectiveUrl)) {
			OBSMessageBox::warning(parent, QTStr("Updater.Title"), effectiveUrl);
		}
	} else if (choice == ACTION_SKIP && !manualUpdate) {
		config_set_string(App()->GetAppConfig(), "General", "SkippedUpdateVersion",
				  QT_TO_UTF8(latestVersion));
		config_save_safe(App()->GetAppConfig(), "tmp", nullptr);
	}
}

} // namespace

void CancelAndroidUpdateChecks()
{
	g_androidShutdownRequested.store(true);
	QJniObject::callStaticMethod<void>("com/obsproject/studio/ObsUpdateHost", "cancelChecks", "()V");
}
#endif
struct QCefCookieManager;

extern QCef *cef;
extern QCefCookieManager *panel_cookies;

using namespace std;

namespace {

QPointer<OBSWhatsNew> obsWhatsNew;

template<typename OBSRef> struct SignalContainer {
	OBSRef ref;
	vector<shared_ptr<OBSSignal>> handlers;
};
} // namespace

void OBSBasic::ReceivedIntroJson(const std::string &text)
{
#ifdef WHATSNEW_ENABLED
	if (isClosing()) {
		return;
	}

	WhatsNewList items;
	try {
		nlohmann::json json = nlohmann::json::parse(text);
		items = json.get<WhatsNewList>();
	} catch (nlohmann::json::exception &e) {
		blog(LOG_WARNING, "Parsing whatsnew data failed: %s", e.what());
		return;
	}

	std::string info_url;
	int info_increment = -1;

	/* check to see if there's an info page for this version */
	for (const WhatsNewItem &item : items) {
		if (item.os) {
			WhatsNewPlatforms platforms = *item.os;
#ifdef _WIN32
			if (!platforms.windows) {
				continue;
			}
#elif defined(__APPLE__)
			if (!platforms.macos) {
				continue;
			}
#else
			if (!platforms.linux) {
				continue;
			}
#endif
		}

		int major = 0;
		int minor = 0;

		sscanf(item.version.c_str(), "%d.%d", &major, &minor);
		if (major == LIBOBS_API_MAJOR_VER && minor == LIBOBS_API_MINOR_VER &&
		    item.RC == OBS_RELEASE_CANDIDATE && item.Beta == OBS_BETA) {
			info_url = item.url;
			info_increment = item.increment;
		}
	}

	/* this version was not found, or no info for this version */
	if (info_increment == -1) {
		return;
	}

#if OBS_RELEASE_CANDIDATE > 0
	constexpr const char *lastInfoVersion = "InfoLastRCVersion";
#elif OBS_BETA > 0
	constexpr const char *lastInfoVersion = "InfoLastBetaVersion";
#else
	constexpr const char *lastInfoVersion = "InfoLastVersion";
#endif
	constexpr uint64_t currentVersion = (uint64_t)LIBOBS_API_VER << 16ULL | OBS_RELEASE_CANDIDATE << 8ULL |
					    OBS_BETA;
	uint64_t lastVersion = config_get_uint(App()->GetAppConfig(), "General", lastInfoVersion);
	int current_version_increment = -1;

	if ((lastVersion & ~0xFFFF0000ULL) < (currentVersion & ~0xFFFF0000ULL)) {
		config_set_int(App()->GetAppConfig(), "General", "InfoIncrement", -1);
		config_set_uint(App()->GetAppConfig(), "General", lastInfoVersion, currentVersion);
	} else {
		current_version_increment = config_get_int(App()->GetAppConfig(), "General", "InfoIncrement");
	}

	if (info_increment <= current_version_increment) {
		return;
	}

	config_set_int(App()->GetAppConfig(), "General", "InfoIncrement", info_increment);
	config_save_safe(App()->GetAppConfig(), "tmp", nullptr);

	cef->init_browser();

	WhatsNewBrowserInitThread *wnbit = new WhatsNewBrowserInitThread(QT_UTF8(info_url.c_str()));

	connect(wnbit, &WhatsNewBrowserInitThread::Result, this, &OBSBasic::ShowWhatsNew, Qt::QueuedConnection);

	whatsNewInitThread.reset(wnbit);
	whatsNewInitThread->start();

#else
	UNUSED_PARAMETER(text);
#endif
}

void OBSBasic::ShowWhatsNew(const QString &url)
{
#ifdef BROWSER_AVAILABLE
	if (isClosing()) {
		return;
	}

	if (obsWhatsNew) {
		obsWhatsNew->close();
	}

	obsWhatsNew = new OBSWhatsNew(this, QT_TO_UTF8(url));
#else
	UNUSED_PARAMETER(url);
#endif
}

void OBSBasic::TimedCheckForUpdates()
{
	if (App()->IsUpdaterDisabled()) {
		return;
	}
	if (!config_get_bool(App()->GetAppConfig(), "General", "EnableAutoUpdates")) {
		return;
	}

#if defined(ENABLE_SPARKLE_UPDATER)
	CheckForUpdates(false);
#elif _WIN32
	long long lastUpdate = config_get_int(App()->GetAppConfig(), "General", "LastUpdateCheck");
	uint32_t lastVersion = config_get_int(App()->GetAppConfig(), "General", "LastVersion");

	if (lastVersion < LIBOBS_API_VER) {
		lastUpdate = 0;
		config_set_int(App()->GetAppConfig(), "General", "LastUpdateCheck", 0);
	}

	long long t = (long long)time(nullptr);
	long long secs = t - lastUpdate;

	if (secs > UPDATE_CHECK_INTERVAL) {
		CheckForUpdates(false);
	}
#elif defined(__ANDROID__)
	QTimer::singleShot(800, this, [this]() {
		if (g_androidShutdownRequested.load() || isClosing())
			return;
		CheckForUpdates(false);
	});
#endif
}

void OBSBasic::CheckForUpdates(bool manualUpdate)
{
#if _WIN32
	ui->actionCheckForUpdates->setEnabled(false);
	ui->actionRepair->setEnabled(false);

	if (updateCheckThread && updateCheckThread->isRunning()) {
		return;
	}
	updateCheckThread.reset(new AutoUpdateThread(manualUpdate));
	updateCheckThread->start();
#elif defined(ENABLE_SPARKLE_UPDATER)
	ui->actionCheckForUpdates->setEnabled(false);

	if (updateCheckThread && updateCheckThread->isRunning()) {
		return;
	}

	MacUpdateThread *mut = new MacUpdateThread(manualUpdate);
	connect(mut, &MacUpdateThread::Result, this, &OBSBasic::MacBranchesFetched, Qt::QueuedConnection);
	updateCheckThread.reset(mut);
	updateCheckThread->start();
#elif defined(__ANDROID__)
	if (App()->IsUpdaterDisabled() || g_androidShutdownRequested.load() || isClosing()) {
		return;
	}
	bool expected = false;
	if (!g_androidUpdateInFlight.compare_exchange_strong(expected, true)) {
		return;
	}
	if (ui->actionCheckForUpdates) {
		ui->actionCheckForUpdates->setEnabled(false);
	}

	const QString currentVersion = GetAndroidInstalledVersionName();
	auto resultJson = std::make_shared<QString>();
	QThread *worker = QThread::create([resultJson]() {
		const QJniObject repoArg = QJniObject::fromString(QString::fromUtf8(ANDROID_GITHUB_REPO));
		const QJniObject jsonObj = QJniObject::callStaticObjectMethod(
			"com/obsproject/studio/ObsUpdateHost", "fetchLatestRelease",
			"(Ljava/lang/String;)Ljava/lang/String;", repoArg.object<jstring>());
		*resultJson = jsonObj.isValid() ? jsonObj.toString() : QString();
		g_androidUpdateInFlight.store(false);
	});
	worker->setParent(nullptr);
	connect(
		worker, &QThread::finished, this,
		[this, manualUpdate, currentVersion, resultJson]() {
			if (g_androidShutdownRequested.load() || isClosing())
				return;
			updateCheckFinished();
			ShowAndroidUpdateResultDialog(this, manualUpdate, currentVersion, *resultJson);
		},
		Qt::QueuedConnection);
	connect(worker, &QThread::finished, worker, &QObject::deleteLater);
	worker->start();
#else
	UNUSED_PARAMETER(manualUpdate);
#endif
}

void OBSBasic::MacBranchesFetched(const QString &branch, bool manualUpdate)
{
#ifdef ENABLE_SPARKLE_UPDATER
	static OBSSparkle *updater;

	if (!updater) {
		updater = new OBSSparkle(QT_TO_UTF8(branch), ui->actionCheckForUpdates);
		return;
	}

	updater->setBranch(QT_TO_UTF8(branch));
	updater->checkForUpdates(manualUpdate);
#else
	UNUSED_PARAMETER(branch);
	UNUSED_PARAMETER(manualUpdate);
#endif
}
