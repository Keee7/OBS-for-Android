#include "OBSAbout.hpp"

#include <widgets/OBSBasic.hpp>
#include <utility/RemoteTextThread.hpp>

#include <qt-wrappers.hpp>

#include <json11.hpp>

#ifdef __ANDROID__
#include <utility/android-window-fit.hpp>

#include <QDesktopServices>
#include <QJniObject>
#include <QPushButton>
#include <QScrollBar>
#include <QUrl>
#include <QtGlobal>

#include <algorithm>
#include <android/api-level.h>
#endif

#include "moc_OBSAbout.cpp"

using namespace json11;

extern bool steam;

#ifdef __ANDROID__
namespace {

QString GetAndroidAboutVersionName()
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
		version = QStringLiteral("0.2.6");
	return version;
}

void OpenAndroidExternalUrl(const QString &url)
{
	const QString target = url.trimmed();
	if (target.isEmpty() || target == QLatin1String("#"))
		return;
	const QJniObject activity = QJniObject::callStaticObjectMethod(
		"org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
	if (activity.isValid()) {
		const QJniObject urlObj = QJniObject::fromString(target);
		const jboolean opened = QJniObject::callStaticMethod<jboolean>(
			"com/obsproject/studio/ObsUpdateHost", "openUrl",
			"(Landroid/content/Context;Ljava/lang/String;)Z", activity.object(), urlObj.object<jstring>());
		if (opened == JNI_TRUE)
			return;
	}
	QDesktopServices::openUrl(QUrl(target));
}

QString AndroidAbiLabel()
{
#if defined(__aarch64__)
	return QStringLiteral("arm64-v8a");
#elif defined(__arm__)
	return QStringLiteral("armeabi-v7a");
#elif defined(__x86_64__)
	return QStringLiteral("x86_64");
#elif defined(__i386__)
	return QStringLiteral("x86");
#else
	return QStringLiteral("android");
#endif
}

QString AndroidLinkHtml(const QString &url, const QString &label)
{
	return QStringLiteral("<a href=\"%1\" style=\"text-decoration:none;\">%2</a>")
		.arg(url.toHtmlEscaped(), label.toHtmlEscaped());
}

} // namespace
#endif

OBSAbout::OBSAbout(QWidget *parent) : QDialog(parent), ui(new Ui::OBSAbout)
{
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	ui->setupUi(this);

#ifdef __ANDROID__
	setWindowTitle(QTStr("About.Android.Title"));
	setMinimumSize(0, 0);

	const QString appVersion = GetAndroidAboutVersionName();
	const QString obsVer = QT_UTF8(obs_get_version_string());
	const QString abi = AndroidAbiLabel();

	ui->name->setText(QStringLiteral("OBS for Android"));
	ui->version->setText(QStringLiteral("v%1 · OBS Studio %2 (%3)").arg(appVersion, obsVer, abi));
	ui->info->setText(QTStr("About.Android.Info"));
	ui->contribute->setText(QTStr("About.Android.ProjectLinks"));

	ui->donate->setWordWrap(true);
	ui->donate->setOpenExternalLinks(false);
	ui->donate->setTextInteractionFlags(Qt::TextBrowserInteraction);
	ui->donate->setText(
		AndroidLinkHtml(QStringLiteral("https://github.com/Keee7/OBS-for-Android"),
				QTStr("About.Android.GitHubRepo")) +
		QStringLiteral("  ·  ") +
		AndroidLinkHtml(QStringLiteral("https://github.com/Keee7/OBS-for-Android/releases"),
				QTStr("About.Android.Releases")) +
		QStringLiteral("  ·  ") +
		AndroidLinkHtml(QStringLiteral("https://github.com/Keee7/OBS-for-Android/issues"),
				QTStr("About.Android.Issues")));
	connect(ui->donate, &QLabel::linkActivated, this, [](const QString &link) { OpenAndroidExternalUrl(link); });

	ui->getInvolved->setWordWrap(true);
	ui->getInvolved->setOpenExternalLinks(false);
	ui->getInvolved->setTextInteractionFlags(Qt::TextBrowserInteraction);
	ui->getInvolved->setText(
		AndroidLinkHtml(QStringLiteral("https://obsproject.com"), QTStr("About.Android.UpstreamOBS")) +
		QStringLiteral("  ·  ") +
		AndroidLinkHtml(QStringLiteral("https://github.com/cunminF/OBS_for_Android"),
				QTStr("About.Android.OriginalPort")) +
		QStringLiteral("  ·  ") +
		AndroidLinkHtml(QStringLiteral("https://obsproject.com/contribute"), QTStr("About.Donate")));
	connect(ui->getInvolved, &QLabel::linkActivated, this,
		[](const QString &link) { OpenAndroidExternalUrl(link); });

	ui->about->setText(QTStr("About"));
	ui->authors->setText(QTStr("About.Authors"));
	ui->license->setText(QTStr("About.License"));

	ui->name->setProperty("class", "text-heading");
	ui->version->setProperty("class", "text-large");
	ui->about->setProperty("class", "bg-base");
	ui->authors->setProperty("class", "bg-base");
	ui->license->setProperty("class", "bg-base");
	ui->info->setProperty("class", "");

	ui->textBrowser->setOpenLinks(false);
	ui->textBrowser->setOpenExternalLinks(false);
	connect(ui->textBrowser, &QTextBrowser::anchorClicked, this,
		[](const QUrl &url) { OpenAndroidExternalUrl(url.toString()); });

	connect(ui->about, &ClickableLabel::clicked, this, &OBSAbout::ShowAbout);
	connect(ui->authors, &ClickableLabel::clicked, this, &OBSAbout::ShowAuthors);
	connect(ui->license, &ClickableLabel::clicked, this, &OBSAbout::ShowLicense);

	const QRect avail = androidAvailableWindowRect(this);
	const bool compact = OBSAndroidLayout::compact(avail.width(), avail.height());

	const int iconSize = compact ? 72 : 112;
	ui->icon->setMinimumSize(iconSize, iconSize);
	ui->icon->setMaximumSize(iconSize, iconSize);
	ui->horizontalLayout_2->setAlignment(ui->icon, Qt::AlignTop | Qt::AlignHCenter);
	if (compact) {
		ui->horizontalLayout_2->setContentsMargins(16, 10, 14, 6);
		ui->horizontalLayout_2->setSpacing(16);
		ui->verticalSpacer->changeSize(10, 6, QSizePolicy::Minimum, QSizePolicy::Fixed);
		ui->verticalSpacer_2->changeSize(10, 4, QSizePolicy::Minimum, QSizePolicy::Fixed);
		ui->verticalSpacer_3->changeSize(10, 6, QSizePolicy::Minimum, QSizePolicy::Fixed);
		ui->widget->setMinimumHeight(40);
		ui->horizontalLayout->setContentsMargins(6, 2, 12, 4);
	} else {
		ui->horizontalLayout_2->setContentsMargins(24, 14, 20, 10);
		ui->horizontalLayout_2->setSpacing(22);
		ui->verticalSpacer->changeSize(12, 10, QSizePolicy::Minimum, QSizePolicy::Fixed);
		ui->verticalSpacer_2->changeSize(12, 8, QSizePolicy::Minimum, QSizePolicy::Fixed);
		ui->verticalSpacer_3->changeSize(12, 10, QSizePolicy::Minimum, QSizePolicy::Fixed);
		ui->widget->setMinimumHeight(48);
		ui->horizontalLayout->setContentsMargins(8, 4, 16, 6);
	}
	ui->horizontalLayout->setSpacing(8);

	auto *closeBtn = new QPushButton(QTStr("Close"), ui->widget);
	closeBtn->setMinimumHeight(compact ? 32 : 36);
	closeBtn->setMinimumWidth(compact ? 72 : 84);
	connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
	ui->horizontalLayout->addWidget(closeBtn);
	ui->horizontalLayout->setStretch(0, 1);
	ui->horizontalLayout->setStretch(1, 1);
	ui->horizontalLayout->setStretch(2, 1);
	ui->horizontalLayout->setStretch(3, 0);

	ShowAbout();

	if (!avail.isEmpty()) {
		const int w = compact ? std::min(740, std::max(300, avail.width() - 20))
				      : std::min(780, std::max(360, avail.width() - 48));
		const int h = compact ? std::min(360, std::max(240, avail.height() - 16))
				      : std::min(520, std::max(320, avail.height() - 48));
		resize(w, h);
	}
	androidFitTopLevelToScreen(this);
	if (!avail.isEmpty() && width() <= avail.width() && height() <= avail.height()) {
		move(avail.center() - QPoint(width() / 2, height() / 2));
	}
#else
	QString bitness;

	if (sizeof(void *) == 4) {
		bitness = " (32 bit)";
	} else if (sizeof(void *) == 8) {
		bitness = " (64 bit)";
	}

	QString ver = obs_get_version_string();

	ui->version->setText(ver + bitness);

	ui->contribute->setText(QTStr("About.Contribute"));

	if (steam) {
		delete ui->donate;
	} else {
		ui->donate->setText("&nbsp;&nbsp;<a href='https://obsproject.com/contribute'>" + QTStr("About.Donate") +
				    "</a>");
		ui->donate->setTextInteractionFlags(Qt::TextBrowserInteraction);
		ui->donate->setOpenExternalLinks(true);
	}

	ui->getInvolved->setText("&nbsp;&nbsp;<a href='https://obsproject.com/developer-contributing'>" +
				 QTStr("About.GetInvolved") + "</a>");
	ui->getInvolved->setTextInteractionFlags(Qt::TextBrowserInteraction);
	ui->getInvolved->setOpenExternalLinks(true);

	ui->about->setText("<a href='#'>" + QTStr("About") + "</a>");
	ui->authors->setText("<a href='#'>" + QTStr("About.Authors") + "</a>");
	ui->license->setText("<a href='#'>" + QTStr("About.License") + "</a>");

	ui->name->setProperty("class", "text-heading");
	ui->version->setProperty("class", "text-large");
	ui->about->setProperty("class", "bg-base");
	ui->authors->setProperty("class", "bg-base");
	ui->license->setProperty("class", "bg-base");
	ui->info->setProperty("class", "");

	connect(ui->about, &ClickableLabel::clicked, this, &OBSAbout::ShowAbout);
	connect(ui->authors, &ClickableLabel::clicked, this, &OBSAbout::ShowAuthors);
	connect(ui->license, &ClickableLabel::clicked, this, &OBSAbout::ShowLicense);

	QPointer<OBSAbout> about(this);

	OBSBasic *main = OBSBasic::Get();
	if (main->patronJson.empty() && !main->patronJsonThread) {
		RemoteTextThread *thread =
			new RemoteTextThread("https://obsproject.com/patreon/about-box.json", "application/json");
		QObject::connect(thread, &RemoteTextThread::Result, main, &OBSBasic::UpdatePatronJson);
		QObject::connect(thread, &RemoteTextThread::Result, this, &OBSAbout::ShowAbout);
		main->patronJsonThread.reset(thread);
		thread->start();
	} else {
		ShowAbout();
	}
#endif
}

#ifdef __ANDROID__
void OBSAbout::SetAndroidActiveTab(int index)
{
	const QString activeStyle = QStringLiteral(
		"background-color: rgba(71, 107, 215, 0.24); border-bottom: 2px solid #476BD7; font-weight: bold; padding: 4px 8px;");
	const QString inactiveStyle = QStringLiteral("background-color: transparent; padding: 4px 8px;");

	ui->about->setStyleSheet(index == 0 ? activeStyle : inactiveStyle);
	ui->authors->setStyleSheet(index == 1 ? activeStyle : inactiveStyle);
	ui->license->setStyleSheet(index == 2 ? activeStyle : inactiveStyle);
}
#endif

void OBSAbout::ShowAbout()
{
#ifdef __ANDROID__
	SetAndroidActiveTab(0);

	const QString appVer = GetAndroidAboutVersionName().toHtmlEscaped();
	const QString obsVer = QT_UTF8(obs_get_version_string()).toHtmlEscaped();
	const QString qtRuntime = QString::fromUtf8(qVersion()).toHtmlEscaped();
	const QString qtCompiled = QStringLiteral(QT_VERSION_STR).toHtmlEscaped();
	const QString qtText = (qtRuntime == qtCompiled)
				       ? qtRuntime
				       : QStringLiteral("%1 (compiled %2)").arg(qtRuntime, qtCompiled);
	const QString sysText =
		QStringLiteral("Android API %1 (%2)").arg(android_get_device_api_level()).arg(AndroidAbiLabel());

	QString html;
	html += QStringLiteral("<div style='font-size:13px; line-height:1.45;'>");
	html += QStringLiteral("<p style='margin:0 0 6px 0;'><b>%1</b><br/>").arg(QTStr("About.Android.Section.Build").toHtmlEscaped());
	html += QStringLiteral("• %1: <code>v%2</code><br/>")
			.arg(QTStr("About.Android.Build.AppVersion").toHtmlEscaped(), appVer);
	html += QStringLiteral("• %1: <code>%2</code><br/>")
			.arg(QTStr("About.Android.Build.OBSCore").toHtmlEscaped(), obsVer);
	html += QStringLiteral("• %1: <code>%2</code><br/>")
			.arg(QTStr("About.Android.Build.Qt").toHtmlEscaped(), qtText);
	html += QStringLiteral("• %1: <code>%2</code><br/>")
			.arg(QTStr("About.Android.Build.System").toHtmlEscaped(), sysText.toHtmlEscaped());
	html += QStringLiteral("• %1: <code>OpenGL ES</code></p>")
			.arg(QTStr("About.Android.Build.Graphics").toHtmlEscaped());

	html += QStringLiteral("<p style='margin:6px 0;'><b>%1</b><br/>")
			.arg(QTStr("About.Android.Section.Highlights").toHtmlEscaped());
	html += QStringLiteral("• %1<br/>").arg(QTStr("About.Android.Highlights.Item1").toHtmlEscaped());
	html += QStringLiteral("• %1<br/>").arg(QTStr("About.Android.Highlights.Item2").toHtmlEscaped());
	html += QStringLiteral("• %1</p>").arg(QTStr("About.Android.Highlights.Item3").toHtmlEscaped());

	html += QStringLiteral("<p style='margin:6px 0 0 0;'><b>%1</b><br/>%2</p>")
			.arg(QTStr("About.Android.Section.Attribution").toHtmlEscaped(),
			     QTStr("About.Android.Disclaimer").toHtmlEscaped());
	html += QStringLiteral("</div>");

	ui->textBrowser->setHtml(html);
	if (ui->textBrowser->verticalScrollBar())
		ui->textBrowser->verticalScrollBar()->setValue(0);
	return;
#else
	OBSBasic *main = OBSBasic::Get();

	if (main->patronJson.empty()) {
		return;
	}

	std::string error;
	Json json = Json::parse(main->patronJson, error);
	const Json::array &patrons = json.array_items();
	QString text;

	text += "<h1>Top Patreon contributors:</h1>";
	text += "<p style=\"font-size:16px;\">";
	bool first = true;
	bool top = true;

	for (const Json &patron : patrons) {
		std::string name = patron["name"].string_value();
		std::string link = patron["link"].string_value();
		int amount = patron["amount"].int_value();

		if (top && amount < 5000) {
			text += "</p>";
			top = false;
		} else if (!first) {
			text += "<br/>";
		}

		if (!link.empty()) {
			text += "<a href=\"";
			text += QT_UTF8(link.c_str()).toHtmlEscaped();
			text += "\">";
		}
		text += QT_UTF8(name.c_str()).toHtmlEscaped();
		if (!link.empty()) {
			text += "</a>";
		}

		if (first) {
			first = false;
		}
	}

	ui->textBrowser->setHtml(text);
#endif
}

void OBSAbout::ShowAuthors()
{
#ifdef __ANDROID__
	SetAndroidActiveTab(1);
#endif
	std::string path;
	QString error = QTStr("About.Error").arg("https://github.com/obsproject/obs-studio/blob/master/AUTHORS");

#ifdef __APPLE__
	if (!GetDataFilePath("AUTHORS", path)) {
#else
	if (!GetDataFilePath("authors/AUTHORS", path)) {
#endif
#ifdef __ANDROID__
		ui->textBrowser->setPlainText(QTStr("About.Android.AuthorsHeader") + QStringLiteral("\n\n") + error);
		if (ui->textBrowser->verticalScrollBar())
			ui->textBrowser->verticalScrollBar()->setValue(0);
#else
		ui->textBrowser->setPlainText(error);
#endif
		return;
	}

	ui->textBrowser->setPlainText(QString::fromStdString(path));

	BPtr<char> text = os_quick_read_utf8_file(path.c_str());

	if (!text || !*text) {
#ifdef __ANDROID__
		ui->textBrowser->setPlainText(QTStr("About.Android.AuthorsHeader") + QStringLiteral("\n\n") + error);
		if (ui->textBrowser->verticalScrollBar())
			ui->textBrowser->verticalScrollBar()->setValue(0);
#else
		ui->textBrowser->setPlainText(error);
#endif
		return;
	}

#ifdef __ANDROID__
	ui->textBrowser->setPlainText(QTStr("About.Android.AuthorsHeader") + QStringLiteral("\n\n") + QT_UTF8(text));
	if (ui->textBrowser->verticalScrollBar())
		ui->textBrowser->verticalScrollBar()->setValue(0);
#else
	ui->textBrowser->setPlainText(QT_UTF8(text));
#endif
}

void OBSAbout::ShowLicense()
{
#ifdef __ANDROID__
	SetAndroidActiveTab(2);
#endif
	std::string path;
	QString error = QTStr("About.Error").arg("https://github.com/obsproject/obs-studio/blob/master/COPYING");

	if (!GetDataFilePath("license/gplv2.txt", path)) {
#ifdef __ANDROID__
		ui->textBrowser->setPlainText(QTStr("About.Android.LicenseSummary") + QStringLiteral("\n") + error);
		if (ui->textBrowser->verticalScrollBar())
			ui->textBrowser->verticalScrollBar()->setValue(0);
#else
		ui->textBrowser->setPlainText(error);
#endif
		return;
	}

	BPtr<char> text = os_quick_read_utf8_file(path.c_str());

	if (!text || !*text) {
#ifdef __ANDROID__
		ui->textBrowser->setPlainText(QTStr("About.Android.LicenseSummary") + QStringLiteral("\n") + error);
		if (ui->textBrowser->verticalScrollBar())
			ui->textBrowser->verticalScrollBar()->setValue(0);
#else
		ui->textBrowser->setPlainText(error);
#endif
		return;
	}

#ifdef __ANDROID__
	ui->textBrowser->setPlainText(QTStr("About.Android.LicenseSummary") + QStringLiteral("\n") + QT_UTF8(text));
	if (ui->textBrowser->verticalScrollBar())
		ui->textBrowser->verticalScrollBar()->setValue(0);
#else
	ui->textBrowser->setPlainText(QT_UTF8(text));
#endif
}
