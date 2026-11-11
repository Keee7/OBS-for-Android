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

#include "AndroidRuntimeBootstrap.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QStandardPaths>

#include <android/log.h>

#include <cstdlib>
#include <fstream>
#include <string>

/* 这个函数跑在 OBSApp 构造之前，那时 libobs 的 blog 还没有 Qt 日志接管，
 * 所以直接写 logcat —— 判据要能在 adb logcat 里看到。 */
#define PKG_TAG "32-PKG"
#define PKG(...) __android_log_print(ANDROID_LOG_INFO, PKG_TAG, __VA_ARGS__)

using namespace std;

namespace {

/* 应用私有 files 目录。
 * 首选 QStandardPaths —— 壳工程 obs_smoke.cpp 走的就是它，实测拿到
 * /data/user/0/org.qtproject.example.obs_shell/files。但那次调用发生在 QApplication
 * 已经存在之后，本轮发生在 OBSApp 构造之前，"没有 QCoreApplication 实例时它还给不给值"
 * 是没测过的，所以配一条不依赖 Qt 的退路：Android 上 /proc/self/cmdline 的第一个
 * NUL 结尾串就是包名，私有目录固定是 /data/user/0/<包名>/files。
 * 走的是哪条都会打进日志，别让它变成看不见的分叉。 */
QString privateFilesDir()
{
	const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	if (!appData.isEmpty()) {
		PKG("私有目录来自 QStandardPaths::AppDataLocation");
		return appData;
	}

	PKG("QStandardPaths::AppDataLocation 返回空（此时还没有 QCoreApplication 实例）→ 改走 /proc/self/cmdline");

	ifstream cmdline("/proc/self/cmdline", ios::binary);
	string pkg;
	getline(cmdline, pkg, '\0');
	if (pkg.empty()) {
		PKG("/proc/self/cmdline 也读不出包名 —— 数据无处可解");
		return QString();
	}
	return QStringLiteral("/data/user/0/%1/files").arg(QString::fromStdString(pkg));
}

/* 把资源树 resRoot 下的所有文件按同样的相对路径拷到磁盘 dstRoot。先整目录删掉，
 * 保证每次启动都是干净的：增量拷贝会让人拿着一份改之前的 .effect 去查现在的问题
 * （M1 实测踩过）。 */
bool extractTree(const QString &resRoot, const QString &dstRoot, int *count)
{
	QDir(dstRoot).removeRecursively();
	if (!QDir().mkpath(dstRoot)) {
		PKG("建不出目录 %s", dstRoot.toUtf8().constData());
		return false;
	}

	int n = 0;
	QDirIterator it(resRoot, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
	while (it.hasNext()) {
		const QString src = it.next();
		const QString dst = dstRoot + src.mid(resRoot.length());
		QDir().mkpath(QFileInfo(dst).absolutePath());
		QFile::remove(dst);
		if (!QFile::copy(src, dst)) {
			PKG("拷贝失败 %s -> %s", src.toUtf8().constData(), dst.toUtf8().constData());
			return false;
		}
		++n;
	}
	*count = n;
	return true;
}

void unpack(const char *label, const QString &resRoot, const QString &dstRoot)
{
	int files = 0;
	const bool ok = extractTree(resRoot, dstRoot, &files);
	PKG("%s：%s，%d 个文件 -> %s", label, ok ? "解包成功" : "解包失败", files, dstRoot.toUtf8().constData());
}

} // namespace

void PrepareAndroidRuntimePaths()
{
	const QString files = privateFilesDir();
	if (files.isEmpty()) {
		PKG("FAIL 拿不到应用私有目录，OBS_ROOT_PATH 未设置");
		return;
	}

	/* 与壳工程同一个布局约定（libobs/obs-android.c 顶部注释）：
	 *   $ROOT/share/libobs/                 libobs 的 .effect / region.ini
	 *   $ROOT/share/obs/obs-studio/         前端数据（locale / themes / images）
	 *   $ROOT/share/obs/obs-plugins/<模块>/ 插件数据（本轮还没打插件，见 os-android.cmake）
	 *   $ROOT/lib/                          插件 .so（同上） */
	const QString root = files + "/obsroot";
	PKG("OBS_ROOT_PATH = %s", root.toUtf8().constData());

	if (!QDir().mkpath(root + "/lib"))
		PKG("FAIL 建不出 %s/lib", root.toUtf8().constData());

	unpack("libobs 数据 :/obsdata", QStringLiteral(":/obsdata"), root + "/share/libobs");
	unpack("前端数据 :/obsfrontenddata", QStringLiteral(":/obsfrontenddata"), root + "/share/obs/obs-studio");
	unpack("插件数据 :/obsplugindata", QStringLiteral(":/obsplugindata"), root + "/share/obs/obs-plugins");

	setenv("OBS_ROOT_PATH", root.toUtf8().constData(), 1);

	/* 判据不是"拷贝返回了 true"，而是消费者按各自的拼法真能看见这几个文件。 */
	struct Probe {
		const char *what;
		const char *rel;
	};
	static const Probe probes[] = {
		{"libobs 渲染要的第一张 effect", "share/libobs/default.effect"},
		{"前端 locale 清单", "share/obs/obs-studio/locale.ini"},
		{"默认语言翻译", "share/obs/obs-studio/locale/en-US.ini"},
	};
	for (const Probe &p : probes) {
		const QString path = root + '/' + p.rel;
		PKG("探针 %s：%s → %s", p.what, path.toUtf8().constData(), QFile::exists(path) ? "在" : "缺失");
	}

	/* 插件数据没有写死的文件名可探（哪些插件在包里由 plugins/CMakeLists.txt 的白名单决定），
	 * 所以探消费者自己拼出来的那个目录：obs-module.c 的 make_data_directory() 把
	 * <root>/share/obs/obs-plugins/%module% 里的 %module% 换成模块名当 data_path，
	 * obs_find_module_file() 再往里 stat 具体文件 —— 目录在不在、里有几张 effect，
	 * 就是这条链能不能走通的证据。 */
	QDir plugRoot(root + "/share/obs/obs-plugins");
	for (const QString &mod : plugRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
		const QDir m(plugRoot.filePath(mod));
		const QStringList files = m.entryList(QDir::Files, QDir::Name);
		PKG("探针插件数据 %s → %d 个顶层文件：%s", m.absolutePath().toUtf8().constData(), int(files.size()),
		    files.join(' ').toUtf8().constData());
	}
}

namespace {

constexpr const char *manifestPath = "assets:/obs-plugins/MANIFEST";

struct BundledPlugin {
	QString name;
	QByteArray digest;
	qint64 size;
};

bool extractPlugin(const BundledPlugin &plugin, const QString &directory)
{
	QFile source(QStringLiteral("assets:/obs-plugins/") + plugin.name);
	QFile target(directory + '/' + plugin.name);
	if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly))
		return false;

	QCryptographicHash hash(QCryptographicHash::Sha256);
	qint64 total = 0;
	while (!source.atEnd()) {
		const QByteArray bytes = source.read(128 * 1024);
		if (bytes.isEmpty() || target.write(bytes) != bytes.size())
			return false;
		hash.addData(bytes);
		total += bytes.size();
	}
	return target.flush() && total == plugin.size && hash.result().toHex() == plugin.digest;
}

} // namespace

bool EnsureAndroidPluginBinaries()
{
	const char *root = getenv("OBS_ROOT_PATH");
	if (!root || !*root)
		return false;

	QFile manifest(manifestPath);
	if (!manifest.open(QIODevice::ReadOnly))
		return false;
	const QByteArray manifestBytes = manifest.readAll();

	static const QRegularExpression entry(
		QStringLiteral(R"(^([a-zA-Z0-9_][a-zA-Z0-9_.-]*[.]so) ([0-9a-f]{64}) ([1-9][0-9]*)$)"));
	QList<BundledPlugin> plugins;
	QSet<QString> seen;
	for (const QString &line : QString::fromLatin1(manifestBytes).split('\n', Qt::SkipEmptyParts)) {
		const auto match = entry.match(line);
		const QString name = match.captured(1);
		bool sizeValid = false;
		const qint64 size = match.captured(3).toLongLong(&sizeValid);
		if (!match.hasMatch() || !sizeValid || seen.contains(name))
			return false;
		seen.insert(name);
		plugins.append({name, match.captured(2).toLatin1(), size});
	}
	if (plugins.isEmpty())
		return false;

	const QString bundles = QString::fromUtf8(root) + "/bundled-plugins";
	const QString digest =
		QString::fromLatin1(QCryptographicHash::hash(manifestBytes, QCryptographicHash::Sha256).toHex());
	const QString directory = bundles + '/' + digest;
	if (!QDir().mkpath(bundles))
		return false;

	QLockFile lock(bundles + "/.extract-lock");
	if (!lock.tryLock(5000))
		return false;

	bool complete = QFileInfo(directory + "/.complete").isFile();
	for (const auto &plugin : plugins) {
		const QFileInfo file(directory + '/' + plugin.name);
		if (!file.isFile() || file.isSymLink() || file.size() != plugin.size) {
			complete = false;
			break;
		}
	}

	if (!complete) {
		QTemporaryDir staging(bundles + "/.staging-XXXXXX");
		if (!staging.isValid())
			return false;
		for (const auto &plugin : plugins) {
			if (!extractPlugin(plugin, staging.path())) {
				PKG("Cannot extract or verify bundled plugin %s", qPrintable(plugin.name));
				return false;
			}
		}
		QFile marker(staging.path() + "/.complete");
		if (!marker.open(QIODevice::WriteOnly))
			return false;
		marker.close();

		// Publish the whole directory only after every plugin is complete.
		if (QFileInfo::exists(directory) && !QDir(directory).removeRecursively())
			return false;
		if (!QDir().rename(staging.path(), directory))
			return false;
	}

	// Earlier test builds wrote bundled modules into the user plugin directory.
	// Migrate only entries explicitly listed as managed by those builds.
	const QString userPlugins = QString::fromUtf8(root) + "/lib/";
	QFile managed(userPlugins + ".managed");
	if (managed.exists()) {
		if (!managed.open(QIODevice::ReadOnly))
			return false;
		static const QRegularExpression filename(
			QStringLiteral(R"(^[a-zA-Z0-9_][a-zA-Z0-9_.-]*[.]so$)"));
		const auto names = QString::fromUtf8(managed.readAll()).split('\n', Qt::SkipEmptyParts);
		managed.close();
		for (const QString &line : names) {
			const QString name = line.trimmed();
			if (!filename.match(name).hasMatch())
				continue;
			const QString path = userPlugins + name;
			if (QFileInfo::exists(path) && !QFile::remove(path))
				return false;
		}
		if (!managed.remove())
			return false;
	}

	PKG("Bundled plugins ready: %s (%d)", qPrintable(digest), int(plugins.size()));
	return setenv("OBS_ANDROID_BUNDLED_PLUGIN_DIR", directory.toUtf8().constData(), 1) == 0;
}
