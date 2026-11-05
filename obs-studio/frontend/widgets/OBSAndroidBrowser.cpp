/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "OBSAndroidBrowser.hpp"
#include <obs-android.h>
#include <QJniObject>
#include <QJniEnvironment>
#include <cstdint>

static constexpr auto hostClass = "com/obsproject/studio/ObsBrowserHost";
struct BrowserHandle {
	QJniObject object;
};
static void *createBrowser(uint32_t texture, uint32_t width, uint32_t height, const char *url, const char *css)
{
	QJniObject context = QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", "getContext",
								"()Landroid/content/Context;");
	if (!context.isValid()) {
		return nullptr;
	}
	auto jurl = QJniObject::fromString(QString::fromUtf8(url ? url : ""));
	auto jcss = QJniObject::fromString(QString::fromUtf8(css ? css : ""));
	auto object = QJniObject::callStaticObjectMethod(
		hostClass, "create",
		"(Landroid/content/Context;IIILjava/lang/String;Ljava/lang/String;)Lcom/obsproject/studio/ObsBrowserHost;",
		context.object<jobject>(), jint(texture), jint(width), jint(height), jurl.object<jstring>(),
		jcss.object<jstring>());
	if (!object.isValid()) {
		return nullptr;
	}
	return new BrowserHandle{object};
}
static int updateBrowser(void *ptr, float matrix[16])
{
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
static void visibleBrowser(void *ptr, bool visible)
{
	static_cast<BrowserHandle *>(ptr)->object.callMethod<void>("setVisible", "(Z)V", jboolean(visible));
}
static void touchBrowser(void *ptr, int x, int y, int action)
{
	static_cast<BrowserHandle *>(ptr)->object.callMethod<void>("touch", "(III)V", jint(x), jint(y), jint(action));
}
static void destroyBrowser(void *ptr)
{
	auto *h = static_cast<BrowserHandle *>(ptr);
	h->object.callMethod<void>("close");
	delete h;
}
static const obs_android_browser_host host = {createBrowser,  updateBrowser, reloadBrowser,
					      visibleBrowser, touchBrowser,  destroyBrowser};
bool OBSAndroidBrowserInit()
{
	obs_android_set_browser_host(&host);
	return true;
}
void OBSAndroidBrowserShutdown()
{
	obs_android_set_browser_host(nullptr);
}
