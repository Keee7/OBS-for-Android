/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "OBSAndroidBrowser.hpp"
#include <obs-android.h>
#include <QJniObject>
#include <QJniEnvironment>
#include <utility/AndroidJniContext.hpp>
#include <android/hardware_buffer_jni.h>
#include <climits>
#include <cstdint>

static constexpr auto hostClass = "com/obsproject/studio/ObsBrowserHost";
struct BrowserHandle {
	QJniObject object;
	jshortArray audioScratch = nullptr;
	jsize audioScratchShorts = 0;
	jintArray audioFormat = nullptr;
};
static void *createBrowser(uint32_t texture, uint32_t width, uint32_t height, const char *url, const char *css,
			   bool transparent, bool rerouteAudio, bool desktopMode, bool fitWidth)
{
	QJniObject context = obsAndroidContext();
	if (!context.isValid()) {
		return nullptr;
	}
	auto jurl = QJniObject::fromString(QString::fromUtf8(url ? url : ""));
	auto jcss = QJniObject::fromString(QString::fromUtf8(css ? css : ""));
	auto object = QJniObject::callStaticObjectMethod(
		hostClass, "create",
		"(Landroid/content/Context;IIILjava/lang/String;Ljava/lang/String;ZZZZ)Lcom/obsproject/studio/ObsBrowserHost;",
		context.object<jobject>(), jint(texture), jint(width), jint(height), jurl.object<jstring>(),
		jcss.object<jstring>(), jboolean(transparent), jboolean(rerouteAudio), jboolean(desktopMode),
		jboolean(fitWidth));
	if (!object.isValid()) {
		return nullptr;
	}
	return new BrowserHandle{object};
}
/* Reader mode: the Java side keeps the Image alive for two acquisitions, so the
 * AHardwareBuffer stays valid while OBS binds and samples it. Nothing is copied
 * on the CPU. */
static void *takeBrowserBuffer(void *ptr)
{
	auto *h = static_cast<BrowserHandle *>(ptr);
	QJniEnvironment env;
	if (!env.jniEnv())
		return nullptr;
	QJniObject buffer = h->object.callMethod<QJniObject>("takePendingBuffer",
							    "()Landroid/hardware/HardwareBuffer;");
	if (!buffer.isValid())
		return nullptr;
	return AHardwareBuffer_fromHardwareBuffer(env.jniEnv(), buffer.object<jobject>());
}
static int updateBrowser(void *ptr, float matrix[16]){
	auto *h = static_cast<BrowserHandle *>(ptr);
	QJniEnvironment env;
	jfloatArray a = env->NewFloatArray(16);
	if (!a) {
		return -1;
	}
	int ok = h->object.callMethod<jint>("update", "([F)I", a);
	if (ok == 1) {
		env->GetFloatArrayRegion(a, 0, 16, matrix);
	}
	env->DeleteLocalRef(a);
	return ok;
}
static void reloadBrowser(void *ptr)
{
	static_cast<BrowserHandle *>(ptr)->object.callMethod<void>("reload");
}
static void goBackBrowser(void *ptr)
{
	static_cast<BrowserHandle *>(ptr)->object.callMethod<void>("goBack");
}
static void visibleBrowser(void *ptr, bool visible)
{
	static_cast<BrowserHandle *>(ptr)->object.callMethod<void>("setVisible", "(Z)V", jboolean(visible));
}
static void touchBrowser(void *ptr, int x, int y, int action)
{
	static_cast<BrowserHandle *>(ptr)->object.callMethod<void>("touch", "(III)V", jint(x), jint(y), jint(action));
}
static void touchMultiBrowser(void *ptr, int action, int x0, int y0, int x1, int y1)
{
	static_cast<BrowserHandle *>(ptr)->object.callMethod<void>("touchMulti", "(IIIII)V", jint(action), jint(x0),
								   jint(y0), jint(x1), jint(y1));
}
static void rightClickBrowser(void *ptr, int x, int y)
{
	static_cast<BrowserHandle *>(ptr)->object.callMethod<void>("rightClick", "(II)V", jint(x), jint(y));
}
static void wheelBrowser(void *ptr, int x, int y, int dx, int dy)
{
	static_cast<BrowserHandle *>(ptr)->object.callMethod<void>("wheel", "(IIII)V", jint(x), jint(y), jint(dx),
								   jint(dy));
}
static uint32_t readBrowserAudio(void *ptr, int16_t *out, uint32_t max_frames, uint32_t *sample_rate,
				 uint32_t *channels)
{
	auto *h = static_cast<BrowserHandle *>(ptr);
	if (!h || !out || max_frames == 0)
		return 0;
	QJniEnvironment env;
	if (!env.jniEnv())
		return 0;
	if (max_frames > uint32_t(INT_MAX / 2))
		max_frames = uint32_t(INT_MAX / 2);
	if (!h->audioFormat) {
		jintArray fmtArr = env->NewIntArray(2);
		if (!fmtArr)
			return 0;
		h->audioFormat = static_cast<jintArray>(env->NewGlobalRef(fmtArr));
		env->DeleteLocalRef(fmtArr);
		if (!h->audioFormat)
			return 0;
	}
	const jsize needShorts = jsize(max_frames * 2);
	if (!h->audioScratch || h->audioScratchShorts < needShorts) {
		if (h->audioScratch) {
			env->DeleteGlobalRef(h->audioScratch);
			h->audioScratch = nullptr;
			h->audioScratchShorts = 0;
		}
		jshortArray scratch = env->NewShortArray(needShorts);
		if (!scratch)
			return 0;
		h->audioScratch = static_cast<jshortArray>(env->NewGlobalRef(scratch));
		env->DeleteLocalRef(scratch);
		if (!h->audioScratch)
			return 0;
		h->audioScratchShorts = needShorts;
	}
	const jint got = h->object.callMethod<jint>("drainAudio", "([SI[I)I", h->audioScratch, jint(max_frames),
						   h->audioFormat);
	if (got <= 0)
		return 0;
	jint fmt[2] = {0, 0};
	env->GetIntArrayRegion(h->audioFormat, 0, 2, fmt);
	if (fmt[0] <= 0 || (fmt[1] != 1 && fmt[1] != 2))
		return 0;
	uint32_t frames = uint32_t(got);
	if (frames > max_frames)
		frames = max_frames;
	env->GetShortArrayRegion(h->audioScratch, 0, jsize(frames * uint32_t(fmt[1])), reinterpret_cast<jshort *>(out));
	if (sample_rate)
		*sample_rate = uint32_t(fmt[0]);
	if (channels)
		*channels = uint32_t(fmt[1]);
	return frames;
}
static void destroyBrowser(void *ptr)
{
	auto *h = static_cast<BrowserHandle *>(ptr);
	h->object.callMethod<void>("close");
	QJniEnvironment env;
	if (env.jniEnv()) {
		if (h->audioScratch)
			env->DeleteGlobalRef(h->audioScratch);
		if (h->audioFormat)
			env->DeleteGlobalRef(h->audioFormat);
	}
	h->audioScratch = nullptr;
	h->audioFormat = nullptr;
	delete h;
}
static const obs_android_browser_host host = {
	createBrowser,     takeBrowserBuffer, updateBrowser, reloadBrowser,  goBackBrowser,   visibleBrowser,
	touchBrowser,      touchMultiBrowser, rightClickBrowser, wheelBrowser, destroyBrowser, readBrowserAudio};
bool OBSAndroidBrowserInit()
{
	obs_android_set_browser_host(&host);
	return true;
}
void OBSAndroidBrowserShutdown()
{
	obs_android_set_browser_host(nullptr);
}
