#include "OBSAndroidPath.hpp"

#include "OBSAndroidDisplay.hpp" // obsAndroidDisplayLog：与 3-3/3-4 其余探针同一个 tag，一条 logcat 看全

#include <OBSApp.hpp>
#include <qt-wrappers.hpp> // OBSMessageBox

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJniObject>
#include <QSaveFile>
#include <obs-android.h>

static constexpr auto storageClass = "com/obsproject/studio/ObsStorageHost";
static QJniObject storageContext()
{
	return QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", "getContext",
						"()Landroid/content/Context;");
}

static int openDocument(const char *path, const char *mode)
{
	auto context = storageContext();
	if (!context.isValid())
		return -1;
	auto uri = QJniObject::fromString(QString::fromUtf8(path));
	auto access = QJniObject::fromString(QString::fromUtf8(mode));
	return QJniObject::callStaticMethod<jint>(storageClass, "open",
		"(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)I",
		context.object<jobject>(), uri.object<jstring>(), access.object<jstring>());
}

void obsAndroidStorageInit()
{
	obs_android_set_storage_open(openDocument);
}

bool obsAndroidStorageManaged(const QString &path)
{
	auto p = QJniObject::fromString(path);
	return QJniObject::callStaticMethod<jboolean>(storageClass, "managed", "(Ljava/lang/String;)Z",
						    p.object<jstring>());
}

bool obsAndroidStorageWritable(const QString &path)
{
	auto context = storageContext();
	if (!context.isValid())
		return false;
	auto p = QJniObject::fromString(path);
	return QJniObject::callStaticMethod<jboolean>(storageClass, "writable",
		"(Landroid/content/Context;Ljava/lang/String;)Z", context.object<jobject>(), p.object<jstring>());
}

QString obsAndroidStorageCreate(const QString &path, const QString &name)
{
	auto context = storageContext();
	if (!context.isValid())
		return {};
	auto p = QJniObject::fromString(path);
	auto n = QJniObject::fromString(name);
	return QJniObject::callStaticObjectMethod(storageClass, "create",
		"(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
		context.object<jobject>(), p.object<jstring>(), n.object<jstring>()).toString();
}

qint64 obsAndroidStorageSpace(const QString &path)
{
	auto context = storageContext();
	if (!context.isValid())
		return -1;
	auto p = QJniObject::fromString(path);
	return QJniObject::callStaticMethod<jlong>(storageClass, "space",
		"(Landroid/content/Context;Ljava/lang/String;)J", context.object<jobject>(), p.object<jstring>());
}

void obsAndroidShowLocalPath(QWidget *parent, const QString &title, const QString &path)
{
	QClipboard *clipboard = QApplication::clipboard();
	clipboard->setText(path);

	// 回读自证：Android 上剪贴板写入可能被平台规则挡掉，所以"已复制"这句话只在回读一致时才说。
	const bool copied = (clipboard->text(QClipboard::Clipboard) == path);
	obsAndroidDisplayLog("3-4 openUrl 替代：%s → %s | 剪贴板回读=%s", title.toUtf8().constData(),
			     path.toUtf8().constData(), copied ? "一致" : "不一致");

	const QString body = copied ? QTStr("Android.PathDialog.CopiedText").arg(path)
				    : QTStr("Android.PathDialog.Text").arg(path);
	OBSMessageBox::information(parent, title, body);
}

void obsAndroidExportLog(QWidget *parent, const char *name)
{
	const QString title = QTStr("LogExport.Title");
	const auto showError = [parent, &title](const QString &message) {
		OBSMessageBox::warning(parent, title, QTStr("LogExport.Error").arg(message));
	};
	char logDir[512];
	if (!name || !*name || GetAppConfigPath(logDir, sizeof(logDir), "obs-studio/logs") <= 0) {
		showError(QTStr("LogExport.NoLogFile"));
		return;
	}

	const QString fileName = QString::fromUtf8(name);
	QFile source(QDir(QString::fromUtf8(logDir)).filePath(fileName));
	if (!source.open(QIODevice::ReadOnly)) {
		showError(source.errorString());
		return;
	}

	const QUrl suggested = QUrl::fromLocalFile(QDir::home().filePath(fileName));
	const QUrl destination = QFileDialog::getSaveFileUrl(parent, title, suggested, QTStr("LogExport.Filter"));
	if (destination.isEmpty()) {
		return;
	}

	QSaveFile output(destination.isLocalFile() ? destination.toLocalFile() : destination.toString(QUrl::FullyEncoded));
	output.setDirectWriteFallback(true);
	if (!output.open(QIODevice::WriteOnly)) {
		showError(output.errorString());
		return;
	}

	const QByteArray utf8Bom = QByteArray::fromHex("efbbbf");
	if (source.peek(utf8Bom.size()) != utf8Bom && output.write(utf8Bom) != utf8Bom.size()) {
		showError(output.errorString());
		return;
	}
	while (true) {
		const QByteArray bytes = source.read(64 * 1024);
		if (bytes.isEmpty()) {
			break;
		}
		if (output.write(bytes) != bytes.size()) {
			showError(output.errorString());
			return;
		}
	}
	if (source.error() != QFileDevice::NoError) {
		showError(source.errorString());
		return;
	}
	if (!output.commit()) {
		showError(output.errorString());
		return;
	}
	OBSMessageBox::information(parent, title, QTStr("LogExport.Success"));
}
