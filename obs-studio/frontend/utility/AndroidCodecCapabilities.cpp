#include "AndroidCodecCapabilities.hpp"

#include <obs.h>
#include <obs-android.h>
#include <QJniEnvironment>
#include <QJniObject>
#include <QByteArray>

static QByteArray findCodec(const char *mime, uint32_t width, uint32_t height, uint32_t num, uint32_t den,
			   uint32_t index = 0, uint32_t bitrate = 0, int mode = 2)
{
	if (!width || !height || !num || !den)
		return {};
	QJniEnvironment env;
	if (!env.isValid())
		return {};
	auto type = QJniObject::fromString(QString::fromUtf8(mime));
	const QJniObject list = QJniObject::callStaticObjectMethod(
		"com/obsproject/studio/ObsCodecCapabilities", "findEncodersMode", "(Ljava/lang/String;IIDII)[Ljava/lang/String;",
		type.object<jstring>(), jint(width), jint(height), jdouble(num) / double(den), jint(bitrate), jint(mode));
	if (env->ExceptionCheck()) {
		env->ExceptionDescribe();
		env->ExceptionClear();
		return {};
	}
	if (!list.isValid())
		return {};
	auto array = list.object<jobjectArray>();
	if (index >= uint32_t(env->GetArrayLength(array)))
		return {};
	auto item = static_cast<jstring>(env->GetObjectArrayElement(array, jsize(index)));
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return {};
	}
	if (!item)
		return {};
	QJniObject name(item);
	env->DeleteLocalRef(item);
	return name.toString().toUtf8();
}

bool OBSAndroidAvcAvailable(uint32_t width, uint32_t height, uint32_t num, uint32_t den)
{
	return !findCodec("video/avc", width, height, num, den).isEmpty();
}

static void queryCodec(void *, calldata_t *cd)
{
	const auto width = calldata_int(cd, "width");
	const auto height = calldata_int(cd, "height");
	const auto num = calldata_int(cd, "fps_num");
	const auto den = calldata_int(cd, "fps_den");
	const auto index = calldata_int(cd, "index");
	const auto bitrate = calldata_int(cd, "bitrate");
	const auto mode = calldata_int(cd, "mode");
	const char *mime = calldata_string(cd, "mime");
	QByteArray name;
	if (width > 0 && height > 0 && num > 0 && den > 0 &&
	    width <= INT32_MAX && height <= INT32_MAX && num <= INT32_MAX && den <= INT32_MAX &&
	    index >= 0 && index <= INT32_MAX && bitrate >= 0 && bitrate <= INT32_MAX && mode >= 0 && mode <= 2 && mime &&
	    (strcmp(mime, "video/avc") == 0 || strcmp(mime, "video/hevc") == 0))
		name = findCodec(mime, uint32_t(width), uint32_t(height), uint32_t(num), uint32_t(den), uint32_t(index), uint32_t(bitrate), int(mode));
	calldata_set_string(cd, "name", name.constData());
	blog(LOG_INFO, "MediaCodec %s %lldx%lld %lld/%lld bitrate=%lld index=%lld: %s",
	     mime ? mime : "unknown", width, height, num, den, bitrate, index,
	     name.isEmpty() ? "end of compatible hardware candidates" : name.constData());
}

static void queryControls(void *, calldata_t *cd)
{
	const char *name = calldata_string(cd, "name");
	const char *mime = calldata_string(cd, "mime");
	const auto mode = calldata_int(cd, "mode");
	calldata_set_bool(cd, "supported", false);
	if (!name || !mime || mode < 0 || mode > 2) return;
	QJniEnvironment env;
	if (!env.isValid()) return;
	auto jname = QJniObject::fromString(QString::fromUtf8(name));
	auto jmime = QJniObject::fromString(QString::fromUtf8(mime));
	auto result = QJniObject::callStaticObjectMethod("com/obsproject/studio/ObsCodecCapabilities", "encoderControls",
		"(Ljava/lang/String;Ljava/lang/String;I)[I", jname.object<jstring>(), jmime.object<jstring>(), jint(mode));
	if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
	if (!result.isValid()) return;
	auto array = result.object<jintArray>();
	if (env->GetArrayLength(array) != 4) return;
	jint values[4];
	env->GetIntArrayRegion(array, 0, 4, values);
	if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
	calldata_set_bool(cd, "supported", true);
	calldata_set_int(cd, "complexity_min", values[0]);
	calldata_set_int(cd, "complexity_max", values[1]);
	calldata_set_int(cd, "quality_min", values[2]);
	calldata_set_int(cd, "quality_max", values[3]);
}

void OBSAndroidRegisterCodecQuery()
{
	QJniEnvironment env;
	JavaVM *vm = nullptr;
	if (env.isValid() && env->GetJavaVM(&vm) == JNI_OK)
		obs_android_set_java_vm(vm);

	proc_handler_add(obs_get_proc_handler(),
		"void android_mediacodec_query_controls(string name, string mime, int mode, out bool supported, out int complexity_min, out int complexity_max, out int quality_min, out int quality_max)",
		queryControls, nullptr);
	proc_handler_add(obs_get_proc_handler(),
		"void android_mediacodec_query_component(string mime, int width, int height, int fps_num, int fps_den, int bitrate, int mode, int index, out string name)",
		queryCodec, nullptr);
}
