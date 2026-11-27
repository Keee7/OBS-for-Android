/* SPDX-License-Identifier: GPL-2.0-or-later */
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
#include <QElapsedTimer>
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
#include <dlfcn.h>
#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <string>

/* 这个函数跑在 OBSApp 构造之前，那时 libobs 尚未安装前端日志处理器，
 * 因此直接输出到 logcat 便于启动阶段排查。 */
#define BOOT_TAG "OBS-Bootstrap"
#define BOOT_LOGI(...) __android_log_print(ANDROID_LOG_INFO, BOOT_TAG, __VA_ARGS__)
#define BOOT_LOGW(...) __android_log_print(ANDROID_LOG_WARN, BOOT_TAG, __VA_ARGS__)
#define BOOT_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, BOOT_TAG, __VA_ARGS__)

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
		BOOT_LOGI("应用私有目录来自 QStandardPaths::AppDataLocation");
		return appData;
	}

	BOOT_LOGI("QStandardPaths::AppDataLocation 返回空，回退从 /proc/self/cmdline 解析包名");

	ifstream cmdline("/proc/self/cmdline", ios::binary);
	string pkg;
	getline(cmdline, pkg, '\0');
	if (pkg.empty()) {
		BOOT_LOGE("无法从 /proc/self/cmdline 读取包名，跳过资源解包");
		return QString();
	}
	return QStringLiteral("/data/user/0/%1/files").arg(QString::fromStdString(pkg));
}

/* 为嵌入在 libobs_<abi>.so 中的 Qt 资源生成二进制指纹：
 * :/obsdata、:/obsfrontenddata、:/obsplugindata 全部编译进当前共享库，
 * 只要同一安装路径上的共享库大小、mtime、inode 及应用版本未变，磁盘缓存树即与包内完全一致。 */
QByteArray currentResourceStamp()
{
	Dl_info dl{};
	if (!dladdr(reinterpret_cast<const void *>(&PrepareAndroidRuntimePaths), &dl) || !dl.dli_fname) {
		BOOT_LOGW("dladdr 无法定位当前共享库，本次将执行完整资源解包");
		return {};
	}
	QString statTarget = QString::fromUtf8(dl.dli_fname);
	const qsizetype bang = statTarget.indexOf(QLatin1String("!/"));
	if (bang > 0)
		statTarget.truncate(bang);
	struct stat st{};
	const QByteArray statPath = statTarget.toUtf8();
	if (stat(statPath.constData(), &st) != 0) {
		BOOT_LOGW("stat(%s) 失败，本次将执行完整资源解包", statPath.constData());
		return {};
	}
	return QStringLiteral("v1|%1|%2|%3.%4|%5|%6\n")
		.arg(QString::fromUtf8(dl.dli_fname))
		.arg(static_cast<qint64>(st.st_size))
		.arg(static_cast<qint64>(st.st_mtim.tv_sec))
		.arg(static_cast<qint64>(st.st_mtim.tv_nsec))
		.arg(static_cast<qint64>(st.st_ino))
		.arg(QStringLiteral(OBS_ANDROID_VERSION_NAME))
		.toUtf8();
}

bool resourceProbesPresent(const QString &root)
{
	return QFileInfo(root + "/share/libobs/default.effect").isFile() &&
	       QFileInfo(root + "/share/obs/obs-studio/locale.ini").isFile() &&
	       QFileInfo(root + "/share/obs/obs-studio/locale/en-US.ini").isFile() &&
	       !QDir(root + "/share/obs/obs-plugins").entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
}

/* 把资源树 resRoot 下的所有文件按同样的相对路径拷到磁盘 dstRoot。先整目录删掉，
 * 保证首次/升级解包干净，并用目录集合缓存 mkpath + 无缓冲整块写入减少系统调用并实时暴露写盘错误。 */
bool extractTree(const QString &resRoot, const QString &dstRoot, int *count)
{
	QDir(dstRoot).removeRecursively();
	if (!QDir().mkpath(dstRoot)) {
		BOOT_LOGE("创建目录失败: %s", dstRoot.toUtf8().constData());
		return false;
	}

	QSet<QString> createdDirs;
	createdDirs.insert(dstRoot);

	int n = 0;
	QDirIterator it(resRoot, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
	while (it.hasNext()) {
		const QString src = it.next();
		const QString dst = dstRoot + src.mid(resRoot.length());
		const QString parentDir = QFileInfo(dst).absolutePath();
		if (!createdDirs.contains(parentDir)) {
			if (!QDir().mkpath(parentDir)) {
				BOOT_LOGE("创建子目录失败: %s", parentDir.toUtf8().constData());
				return false;
			}
			createdDirs.insert(parentDir);
		}
		QFile in(src);
		QFile out(dst);
		if (!in.open(QIODevice::ReadOnly) ||
		    !out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Unbuffered)) {
			BOOT_LOGE("复制资源文件失败: %s -> %s", src.toUtf8().constData(), dst.toUtf8().constData());
			return false;
		}
		const QByteArray bytes = in.readAll();
		if (!bytes.isEmpty() && out.write(bytes) != bytes.size()) {
			BOOT_LOGE("写入资源文件失败: %s -> %s", src.toUtf8().constData(), dst.toUtf8().constData());
			return false;
		}
		if (out.error() != QFileDevice::NoError) {
			BOOT_LOGE("刷新资源文件失败: %s -> %s", src.toUtf8().constData(), dst.toUtf8().constData());
			return false;
		}
		out.close();
		++n;
	}
	*count = n;
	return true;
}

bool unpack(const char *label, const QString &resRoot, const QString &dstRoot)
{
	int files = 0;
	const bool ok = extractTree(resRoot, dstRoot, &files);
	if (ok)
		BOOT_LOGI("%s：解包成功（%d 个文件 -> %s）", label, files, dstRoot.toUtf8().constData());
	else
		BOOT_LOGE("%s：解包失败（已处理 %d 个文件 -> %s）", label, files, dstRoot.toUtf8().constData());
	return ok;
}

} // namespace

void PrepareAndroidRuntimePaths()
{
	QElapsedTimer timer;
	timer.start();

	const QString files = privateFilesDir();
	if (files.isEmpty()) {
		BOOT_LOGE("获取应用私有目录失败，未设置 OBS_ROOT_PATH");
		return;
	}

	/* 与壳工程同一个布局约定（libobs/obs-android.c 顶部注释）：
	 *   $ROOT/share/libobs/                 libobs 的 .effect / region.ini
	 *   $ROOT/share/obs/obs-studio/         前端数据（locale / themes / images）
	 *   $ROOT/share/obs/obs-plugins/<模块>/ 插件数据（本轮还没打插件，见 os-android.cmake）
	 *   $ROOT/lib/                          插件 .so（同上） */
	const QString root = files + "/obsroot";
	BOOT_LOGI("OBS_ROOT_PATH = %s", root.toUtf8().constData());

	if (!QDir().mkpath(root + "/lib"))
		BOOT_LOGE("创建插件目录失败: %s/lib", root.toUtf8().constData());

	const QString stampPath = root + "/.resources-stamp";
	const QByteArray expectedStamp = currentResourceStamp();
	bool cacheHit = false;
	if (!expectedStamp.isEmpty() && resourceProbesPresent(root)) {
		QFile stampFile(stampPath);
		if (stampFile.open(QIODevice::ReadOnly) && stampFile.readAll() == expectedStamp)
			cacheHit = true;
	}

	if (cacheHit) {
		setenv("OBS_ROOT_PATH", root.toUtf8().constData(), 1);
		BOOT_LOGI("运行时资源缓存命中，跳过解包（耗时 %lld ms）", static_cast<long long>(timer.elapsed()));
		return;
	}

	QFile::remove(stampPath);
	const bool okLibobs = unpack("libobs 资源 :/obsdata", QStringLiteral(":/obsdata"), root + "/share/libobs");
	const bool okFrontend =
		unpack("前端资源 :/obsfrontenddata", QStringLiteral(":/obsfrontenddata"), root + "/share/obs/obs-studio");
	const bool okPlugins =
		unpack("插件资源 :/obsplugindata", QStringLiteral(":/obsplugindata"), root + "/share/obs/obs-plugins");

	setenv("OBS_ROOT_PATH", root.toUtf8().constData(), 1);

	/* 判据不是"拷贝返回了 true"，而是消费者按各自的拼法真能看见这几个文件。 */
	struct Probe {
		const char *what;
		const char *rel;
	};
	static const Probe probes[] = {
		{"libobs 默认着色器", "share/libobs/default.effect"},
		{"前端语言列表", "share/obs/obs-studio/locale.ini"},
		{"默认语言文件", "share/obs/obs-studio/locale/en-US.ini"},
	};
	bool probesOk = true;
	for (const Probe &p : probes) {
		const QString path = root + '/' + p.rel;
		if (QFile::exists(path)) {
			BOOT_LOGI("资源校验 [%s]：%s（存在）", p.what, path.toUtf8().constData());
		} else {
			probesOk = false;
			BOOT_LOGW("资源校验 [%s]：%s（缺失）", p.what, path.toUtf8().constData());
		}
	}

	if (okLibobs && okFrontend && okPlugins && probesOk && !expectedStamp.isEmpty()) {
		const QString tmpStampPath = stampPath + ".tmp";
		QFile tmpStamp(tmpStampPath);
		if (tmpStamp.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Unbuffered) &&
		    tmpStamp.write(expectedStamp) == expectedStamp.size() &&
		    tmpStamp.error() == QFileDevice::NoError) {
			tmpStamp.close();
			QFile::remove(stampPath);
			if (!QFile::rename(tmpStampPath, stampPath))
				QFile::remove(tmpStampPath);
		} else {
			tmpStamp.close();
			QFile::remove(tmpStampPath);
		}
	}

	QDir plugRoot(root + "/share/obs/obs-plugins");
	for (const QString &mod : plugRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
		const QDir m(plugRoot.filePath(mod));
		const QStringList plugFiles = m.entryList(QDir::Files, QDir::Name);
		BOOT_LOGI("插件数据目录 %s（%d 个顶层文件：%s）", m.absolutePath().toUtf8().constData(),
			  int(plugFiles.size()), plugFiles.join(' ').toUtf8().constData());
	}
	BOOT_LOGI("运行时资源首次/升级解包完成（耗时 %lld ms）", static_cast<long long>(timer.elapsed()));
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
				BOOT_LOGE("Cannot extract or verify bundled plugin %s", qPrintable(plugin.name));
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

	BOOT_LOGI("Bundled plugins ready: %s (%d)", qPrintable(digest), int(plugins.size()));
	return setenv("OBS_ANDROID_BUNDLED_PLUGIN_DIR", directory.toUtf8().constData(), 1) == 0;
}
