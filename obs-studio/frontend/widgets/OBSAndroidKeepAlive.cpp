#include "OBSAndroidKeepAlive.hpp"

#ifdef __ANDROID__

#include <QJniEnvironment>
#include <QJniObject>

#include <mutex>
#include <cstring>

#include <obs.hpp>

/* Java 侧宿主（frontend/cmake/android/java/com/obsproject/studio/ObsForegroundService.java）
 * 全是 public static，和 ObsDisplayHost 一样只走 callStaticMethod。 */
static const char kServiceClass[] = "com/obsproject/studio/ObsForegroundService";

/* 必须是 Activity：QtNative.activity() 是这套里唯一稳定的 Context 来源（同 OBSAndroidDisplay.cpp），
 * Java 侧立刻 getApplicationContext()，不把 Activity 引用留在静态字段上。 */
static QJniObject hostActivity()
{
	return QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", "activity",
						  "()Landroid/app/Activity;");
}

static QString callDescribe()
{
	const QJniObject s = QJniObject::callStaticObjectMethod(kServiceClass, "describe", "()Ljava/lang/String;");
	return s.isValid() ? s.toString() : QString();
}

/* 上一次递给 Java 的状态。前台服务只在"有没有输出在跑"这一维上开关，另加一个状态字符串
 * 只是给通知栏文案去重用的；真正的服务状态以 Java 侧 instance/计数器为准（describe 里能看到）。 */
static std::mutex g_keepAliveMutex;
static bool g_running = false;
static QString g_state;

struct CaptureSources { bool camera = false; bool microphone = false; };

static bool collectCaptureSource(void *data, obs_source_t *source)
{
    auto *sources = static_cast<CaptureSources *>(data);
    if (!obs_source_active(source)) return true;
    const char *id = obs_source_get_id(source);
    if (id && strcmp(id, "android_camera_input") == 0) sources->camera = true;
    if (id && strcmp(id, "android_audio_input") == 0) sources->microphone = true;
    return true;
}

void obsAndroidKeepAliveSetLocale(const char *locale)
{
	const QJniObject jloc = QJniObject::fromString(locale ? QString::fromUtf8(locale) : QString());
	QJniObject::callStaticMethod<void>(kServiceClass, "setAppLocale", "(Ljava/lang/String;)V",
					   jloc.object<jstring>());
}

bool obsAndroidKeepAliveSetRunning(bool running, const char *state, QString *detail)
{
    CaptureSources sources;
    obs_enum_sources(collectCaptureSource, &sources);
    const QJniObject activity = hostActivity();
    const bool capture = activity.isValid() &&
        QJniObject::callStaticMethod<jboolean>(kServiceClass, "setCaptureSources",
            "(Landroid/content/Context;ZZ)Z", activity.object<jobject>(),
            jboolean(sources.camera), jboolean(sources.microphone));
    if (!running && capture) state = "capturing";
    running = running || capture;
	const QString what = state ? QString::fromUtf8(state) : QStringLiteral("running");
	QString jstatus;
	bool repeat = false;

	{
		std::lock_guard<std::mutex> lk(g_keepAliveMutex);
		if (!running && !g_running) {
			if (detail)
				*detail = QStringLiteral("未在运行，跳过停止请求");
			return true;
		}
		/* 3-5 看门狗每秒复核一次：状态没变就别再惊动 Java（重复 startForeground 会每秒重刷通知）。 */
		repeat = running && g_running && g_state == what;
	}

	if (repeat) {
		if (detail)
			*detail = QStringLiteral("已在运行 (%1)，跳过重复启动").arg(what);
		return true;
	}

	const QJniObject act = hostActivity();
	if (!act.isValid()) {
		if (detail)
			*detail = QStringLiteral("无法获取 QtNative.activity()，前台服务启动失败");
		jstatus = obsAndroidKeepAliveStatus();
		blog(LOG_WARNING, "OBSAndroidKeepAlive: 无法获取 Activity，当前状态: %s",
		     jstatus.toUtf8().constData());
		return false;
	}

	bool ok;
	if (running) {
		const QJniObject jwhat = QJniObject::fromString(what);
		ok = QJniObject::callStaticMethod<jboolean>(kServiceClass, "start",
							   "(Landroid/content/Context;Ljava/lang/String;)Z",
							   act.object<jobject>(), jwhat.object<jstring>()) != 0;
	} else {
		ok = QJniObject::callStaticMethod<jboolean>(kServiceClass, "stop", "(Landroid/content/Context;)Z",
							    act.object<jobject>()) != 0;
	}

	/* 成功之后才提交状态。失败时保留旧值，看门狗下一拍会重试 —— 先把 g_running 写成
	 * 目标值再调 Java 的话，失败的那次会被下一拍判成"已在跑"并直接短路，结果是一次
	 * startForeground 失败（通知权限被拒、OEM 限制）就永久没有前台服务，推流/录制
	 * 可以在后台被系统杀掉而不再有任何重试。 */
	if (ok) {
		std::lock_guard<std::mutex> lk(g_keepAliveMutex);
		g_running = running;
		g_state = what;
	}

	jstatus = obsAndroidKeepAliveStatus();
	const QString verb = running ? QStringLiteral("启动前台服务(%1)").arg(what) : QStringLiteral("停止前台服务");
	const QString line = QStringLiteral("%1%2：%3")
				     .arg(verb)
				     .arg(ok ? QStringLiteral("成功") : QStringLiteral("失败"))
				     .arg(jstatus);
	if (detail)
		*detail = line;
	blog(ok ? LOG_INFO : LOG_WARNING, "OBSAndroidKeepAlive: %s", line.toUtf8().constData());
	return ok;
}

void obsAndroidKeepAliveSetScreenOn(bool enabled, bool outputActive)
{
	const QJniObject act = hostActivity();
	if (!act.isValid())
		return;

	bool wantOn = false;
	if (enabled) {
		wantOn = outputActive;
		if (!wantOn) {
			wantOn = QJniObject::callStaticMethod<jboolean>(
					 "com/obsproject/studio/ObsProjectionHost", "projectionActive", "()Z") != 0;
		}
	}

	QJniObject::callStaticMethod<void>(kServiceClass, "setKeepScreenOn", "(Landroid/app/Activity;Z)V",
					   act.object<jobject>(), jboolean(wantOn));
}

QString obsAndroidKeepAliveStatus()
{
	const QString desc = callDescribe();
	if (!desc.isEmpty())
		return desc;
	/* describe() 调不到（类没进 APK / 静态方法没搜到）时至少把 native 侧的意图报出来，
	 * 与"服务在跑但 describe 返回空"是两回事。 */
	return QStringLiteral("Java 状态不可用（describe 返回空，native 期望状态 running=%1）").arg(g_running);
}

#endif // __ANDROID__
