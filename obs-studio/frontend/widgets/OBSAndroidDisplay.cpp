#include "OBSAndroidDisplay.hpp"

#ifdef __ANDROID__

#include <QJniEnvironment>
#include <QObject>
#include <QWindow>

#include <android/log.h>
#include <android/native_window_jni.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include <obs.hpp>

namespace {
constexpr char displayHostClass[] = "com/obsproject/studio/ObsDisplayHost";

struct DisplayTarget {
	QObject *context;
	OBSAndroidDisplay *display;
};

std::mutex targetsMutex;
std::unordered_map<qint64, DisplayTarget> targets;
qint64 nextId = 0;

void nativeSurfaceChanged(JNIEnv *, jclass, jlong id, jobject surface, jint generation)
{
	std::lock_guard lock(targetsMutex);
	const auto target = targets.find(id);
	if (target == targets.end())
		return;

	// Keep both the queue target and the Java Surface alive during handoff.
	// The Android UI thread must never wait for the Qt or OBS render thread.
	QMetaObject::invokeMethod(
		target->second.context,
		[id, surface = QJniObject(surface), generation] {
			OBSAndroidDisplay *display;
			{
				std::lock_guard lock(targetsMutex);
				const auto target = targets.find(id);
				if (target == targets.end())
					return;
				display = target->second.display;
			}
			display->surfaceChanged(surface, generation);
		},
		Qt::QueuedConnection);
}

bool registerSurfaceCallback()
{
	static const bool registered = [] {
		QJniEnvironment env;
		return env.registerNativeMethods(displayHostClass, {
			{ "nativeSurfaceChanged", "(JLandroid/view/Surface;I)V",
			  reinterpret_cast<void *>(nativeSurfaceChanged) },
		});
	}();
	return registered;
}
} // namespace

OBSAndroidDisplay::OBSAndroidDisplay(QObject *context, std::function<void()> changed)
	: changed(std::move(changed))
{
	std::lock_guard lock(targetsMutex);
	id = ++nextId;
	targets.emplace(id, DisplayTarget{context, this});
}

OBSAndroidDisplay::~OBSAndroidDisplay()
{
	{
		std::lock_guard lock(targetsMutex);
		targets.erase(id);
	}
	if (host.isValid())
		host.callMethod<void>("close");
}

bool OBSAndroidDisplay::attach(QWindow *window)
{
	if (host.isValid())
		return true;

	if (!window || !registerSurfaceCallback())
		return false;

	// Qt 6.11's Android QPA exposes its QtWindow ViewGroup as the WId.
	const auto view = reinterpret_cast<jobject>(window->winId());
	if (!view)
		return false;

	parent = QJniObject(view);
	host = QJniObject(displayHostClass, "(Landroid/view/ViewGroup;J)V", parent.object(), jlong(id));
	return host.isValid();
}

void *OBSAndroidDisplay::acquireWindow() const
{
	if (!surface.isValid())
		return nullptr;

	QJniEnvironment env;
	return ANativeWindow_fromSurface(env.jniEnv(), surface.object());
}

void OBSAndroidDisplay::surfaceChanged(const QJniObject &nextSurface, int generation)
{
	surface = nextSurface;
	// DisplayCreated can close the owning widget. Keep the callback and the
	// Java host alive independently of the native display during notification.
	const auto onChanged = changed;
	const auto javaHost = host;
	onChanged();
	// The old EGL swapchain has now been destroyed. Java can release the
	// SurfaceTexture and its producer without abandoning a live buffer queue.
	javaHost.callMethod<void>("releaseRetired", jint(generation));
}

void obsAndroidDisplayLog(const char *fmt, ...)
{
	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	__android_log_print(ANDROID_LOG_INFO, "OBS-display", "%s", buffer);
	blog(LOG_INFO, "OBS-display: %s", buffer);
}

#endif
