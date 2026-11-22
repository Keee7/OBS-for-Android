/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "OBSAndroidAudio.hpp"

#include <obs.h>
#include <obs-android.h>

#include <QByteArray>
#include <QJniObject>
#include <QStringList>

#include <cstring>

/* Java side (frontend/cmake/android/java/com/obsproject/studio/ObsAudioHost.java)
 * exposes only public static methods. */
static constexpr auto hostClass = "com/obsproject/studio/ObsAudioHost";

static QJniObject appContext()
{
	QJniObject ctx;
	const char *const getters[] = {"getContext", "activity"};
	for (const char *g : getters) {
		ctx = QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", g,
						 "()Landroid/content/Context;");
		if (ctx.isValid())
			break;
	}
	return ctx;
}

static void copyField(char *dst, size_t dstLen, const QString &src)
{
	if (!dst || dstLen == 0)
		return;
	const QByteArray utf8 = src.toUtf8();
	const size_t cap = dstLen - 1;
	size_t n = (size_t)utf8.size() <= cap ? (size_t)utf8.size() : cap;
	if (n < (size_t)utf8.size()) {
		while (n > 0 && (static_cast<unsigned char>(utf8.at((qsizetype)n)) & 0xC0) == 0x80)
			n--;
	}
	memcpy(dst, utf8.constData(), n);
	dst[n] = '\0';
}

/* Wire format from snapshot()/snapshotOutputs(): id|type|flag|product|label, entries split by ';'. */
static int getAudioDevicesFromHost(const char *method, bool outputMode, struct obs_android_audio_device *out, int max)
{
	const QJniObject snap =
		QJniObject::callStaticObjectMethod(hostClass, method, "()Ljava/lang/String;");
	if (!snap.isValid())
		return -1; /* bridge missing, not "no devices" */

	const QString joined = snap.toString();
	if (joined.isEmpty())
		return 0;

	const QStringList entries = joined.split(QLatin1Char(';'), Qt::SkipEmptyParts);
	int n = 0;
	int dropped = 0;
	for (const QString &e : entries) {
		if (n >= max)
			break;
		const QStringList f = e.split(QLatin1Char('|'));
		if (f.size() < 5) {
			dropped++;
			continue;
		}
		bool okId = false, okType = false;
		const int id = f.at(0).toInt(&okId);
		const int type = f.at(1).toInt(&okType);
		if (!okId || !okType) {
			dropped++;
			continue;
		}
		const bool available = (f.at(2) == QLatin1String("1"));
		out[n].id = id;
		out[n].type = type;
		out[n].is_source = !outputMode && available;
		out[n].is_sink = outputMode && available;
		copyField(out[n].product, sizeof(out[n].product), f.at(3));
		copyField(out[n].label, sizeof(out[n].label), f.at(4));
		n++;
	}
	if (n == 0 && dropped > 0)
		return -1; /* protocol mismatch, not an empty list */
	return n;
}

static int getAudioDevices(void *param, struct obs_android_audio_device *out, int max)
{
	(void)param;
	return getAudioDevicesFromHost("snapshot", false, out, max);
}

static int getAudioOutputDevices(void *param, struct obs_android_audio_device *out, int max)
{
	(void)param;
	return getAudioDevicesFromHost("snapshotOutputs", true, out, max);
}

static bool hasCapturePermission(void *param)
{
	(void)param;
	return QJniObject::callStaticMethod<jboolean>(hostClass, "hasRecordPermission", "()Z") != 0;
}

static struct obs_android_audio_host g_audioHost = {
	nullptr,
	getAudioDevices,
	hasCapturePermission,
	getAudioOutputDevices,
};

bool OBSAndroidAudioInit()
{
	const QJniObject ctx = appContext();
	if (!ctx.isValid()) {
		blog(LOG_WARNING, "Android audio host: no Context from QtNative");
		return false;
	}
	const jboolean ok = QJniObject::callStaticMethod<jboolean>(hostClass, "install",
								   "(Landroid/content/Context;)Z",
								   ctx.object<jobject>());
	if (!ok) {
		blog(LOG_WARNING, "Android audio host: ObsAudioHost.install() failed");
		return false;
	}
	obs_android_set_audio_host(&g_audioHost);
	blog(LOG_INFO, "Android audio host registered (AAudio input device list)");
	return true;
}

void OBSAndroidAudioShutdown()
{
	obs_android_set_audio_host(nullptr);
}
