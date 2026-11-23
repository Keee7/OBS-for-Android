#include "AndroidCodecCapabilities.hpp"

#include <obs.h>
#include <obs-android.h>
#include <QJniEnvironment>
#include <QJniObject>
#include <QByteArray>

static QByteArray findCodec(const char *mime, uint32_t width, uint32_t height, uint32_t num, uint32_t den,
			   uint32_t index = 0, uint32_t bitrate = 0, int mode = 2, int profile = 0, bool p010 = false)
{
	if (!width || !height || !num || !den)
		return {};
	QJniEnvironment env;
	if (!env.isValid())
		return {};
	auto type = QJniObject::fromString(QString::fromUtf8(mime));
	const QJniObject list = QJniObject::callStaticObjectMethod(
		"com/obsproject/studio/ObsCodecCapabilities", "findEncodersProfile", "(Ljava/lang/String;IIDIIIZ)[Ljava/lang/String;",
		type.object<jstring>(), jint(width), jint(height), jdouble(num) / double(den), jint(bitrate), jint(mode),
		jint(profile), jboolean(p010));
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

static bool hasHardwareEncoder(const char *mime, jint mode, jint profile = 0, bool p010 = false)
{
	const auto type = QJniObject::fromString(QString::fromUtf8(mime));
	return QJniObject::callStaticMethod<jboolean>("com/obsproject/studio/ObsCodecCapabilities",
		"hasHardwareEncoderProfile", "(Ljava/lang/String;IIZ)Z", type.object<jstring>(), mode, profile, jboolean(p010));
}

bool OBSAndroidHevcMain10Available(bool pq, bool cbr)
{
	return obs_get_encoder_codec("android_mediacodec_hevc_main10") &&
	       hasHardwareEncoder("video/hevc", cbr ? 2 : -1, obs_android_hevc_main10_profile(pq), true);
}

bool OBSAndroidCbrEncoderAvailable(const char *id)
{
	const char *codec = obs_get_encoder_codec(id);
	if (!codec)
		return false;
	if (strcmp(codec, "h264") == 0)
		return hasHardwareEncoder("video/avc", 2);
	if (strcmp(id, "android_mediacodec_hevc_main10") == 0)
		return OBSAndroidHevcMain10Available(false, true) || OBSAndroidHevcMain10Available(true, true);
	if (strcmp(codec, "hevc") == 0)
		return hasHardwareEncoder("video/hevc", 2);
	if (strcmp(codec, "av1") == 0)
		return hasHardwareEncoder("video/av01", 2);
	if (strcmp(codec, "vp9") == 0)
		return hasHardwareEncoder("video/x-vnd.on2.vp9", 2);
	return false;
}

video_colorspace OBSAndroidVideoColorSpace()
{
	obs_video_info video;
	return obs_get_video_info(&video) ? video.colorspace : VIDEO_CS_DEFAULT;
}

bool OBSAndroidEncoderUsesSoftwareFallback(const char *id, video_colorspace space, bool cbr)
{
	if (!id || (strcmp(id, "android_mediacodec_hevc") != 0 && strcmp(id, "android_mediacodec_hevc_main10") != 0))
		return false;
	const bool pq = space == VIDEO_CS_2100_PQ;
	const bool hlg = space == VIDEO_CS_2100_HLG;
	return (pq || hlg) && !OBSAndroidHevcMain10Available(pq, cbr) && obs_get_encoder_codec("ffmpeg_x265") != nullptr;
}

const char *OBSAndroidEncoderForVideo(const char *id, bool cbr, bool *fellBackToSoftware)
{
	if (fellBackToSoftware)
		*fellBackToSoftware = false;
	constexpr auto hevcMain10 = "android_mediacodec_hevc_main10";
	if (!id || (strcmp(id, "android_mediacodec_hevc") != 0 && strcmp(id, hevcMain10) != 0))
		return id;
	const auto space = OBSAndroidVideoColorSpace();
	const bool pq = space == VIDEO_CS_2100_PQ;
	if ((pq || space == VIDEO_CS_2100_HLG) && OBSAndroidHevcMain10Available(pq, cbr))
		return hevcMain10;
	if (OBSAndroidEncoderUsesSoftwareFallback(id, space, cbr)) {
		blog(LOG_WARNING,
		     "MediaCodec HEVC Main10 unavailable for the active HDR color space. Using software HEVC (x265).");
		if (fellBackToSoftware)
			*fellBackToSoftware = true;
		return "ffmpeg_x265";
	}
	return id;
}

static void queryEncoderAvailability(void *, calldata_t *cd)
{
	calldata_set_bool(cd, "supported", hasHardwareEncoder(calldata_string(cd, "mime"), -1,
							     jint(calldata_int(cd, "profile")), calldata_bool(cd, "p010")));
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
	const auto profile = calldata_int(cd, "profile");
	const bool p010 = calldata_bool(cd, "p010");
	const char *mime = calldata_string(cd, "mime");
	QByteArray name;
	if (width > 0 && height > 0 && num > 0 && den > 0 &&
	    width <= INT32_MAX && height <= INT32_MAX && num <= INT32_MAX && den <= INT32_MAX &&
	    index >= 0 && index <= INT32_MAX && bitrate >= 0 && bitrate <= INT32_MAX &&
	    profile >= 0 && profile <= INT32_MAX && mode >= 0 && mode <= 2 && mime)
		name = findCodec(mime, uint32_t(width), uint32_t(height), uint32_t(num), uint32_t(den), uint32_t(index),
				 uint32_t(bitrate), int(mode), int(profile), p010);
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

	proc_handler_add(obs_get_proc_handler(), "void android_mediacodec_encoder_available(string mime, int profile, bool p010, out bool supported)",
		queryEncoderAvailability, nullptr);

	proc_handler_add(obs_get_proc_handler(),
		"void android_mediacodec_query_controls(string name, string mime, int mode, out bool supported, out int complexity_min, out int complexity_max, out int quality_min, out int quality_max)",
		queryControls, nullptr);
	proc_handler_add(obs_get_proc_handler(),
		"void android_mediacodec_query_component(string mime, int width, int height, int fps_num, int fps_den, int bitrate, int mode, int index, int profile, bool p010, out string name)",
		queryCodec, nullptr);
}
