/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "AndroidCodecCapabilities.hpp"

#include <obs.h>
#include <obs-android.h>
#include <QJniEnvironment>
#include <QJniObject>
#include <QByteArray>
#include <QStringList>

static QByteArray findCodec(const char *mime, uint32_t width, uint32_t height, uint32_t num, uint32_t den,
			   uint32_t index = 0, uint32_t bitrate = 0, int mode = 2, int profile = 0, bool p010 = false,
			   bool allowByteBuffer = false)
{
	if (!width || !height || !num || !den)
		return {};
	QJniEnvironment env;
	if (!env.isValid())
		return {};
	auto type = QJniObject::fromString(QString::fromUtf8(mime));
	const QJniObject list = QJniObject::callStaticObjectMethod(
		"com/obsproject/studio/ObsCodecCapabilities", "findEncodersProfile",
		"(Ljava/lang/String;IIDIIIZZ)[Ljava/lang/String;", type.object<jstring>(), jint(width), jint(height),
		jdouble(num) / double(den), jint(bitrate), jint(mode), jint(profile), jboolean(p010),
		jboolean(allowByteBuffer));
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

struct CodecCandidateEntry {
	QByteArray tier;
	QByteArray name;
	QByteArray mismatch;
};

static const char *rateModeLabel(int64_t mode)
{
	switch (mode) {
	case 0:
		return "CQ";
	case 1:
		return "VBR";
	case 2:
		return "CBR";
	default:
		return "unknown";
	}
}

static bool findCodecCandidate(const char *mime, uint32_t width, uint32_t height, uint32_t num, uint32_t den,
			       uint32_t index, uint32_t bitrate, int mode, int profile, bool p010,
			       bool allowByteBuffer, CodecCandidateEntry &entry, int &rejectFlags,
			       QByteArray &diagnostics)
{
	entry = {};
	rejectFlags = 0;
	diagnostics.clear();
	if (!width || !height || !num || !den)
		return false;
	QJniEnvironment env;
	if (!env.isValid())
		return false;
	auto type = QJniObject::fromString(QString::fromUtf8(mime));
	const jdouble fps = jdouble(num) / double(den);
	const QJniObject list = QJniObject::callStaticObjectMethod(
		"com/obsproject/studio/ObsCodecCapabilities", "findEncoderCandidates",
		"(Ljava/lang/String;IIDIIIZZ)[Ljava/lang/String;", type.object<jstring>(), jint(width), jint(height),
		fps, jint(bitrate), jint(mode), jint(profile), jboolean(p010), jboolean(allowByteBuffer));
	if (env->ExceptionCheck()) {
		env->ExceptionDescribe();
		env->ExceptionClear();
		return false;
	}
	rejectFlags = int(QJniObject::callStaticMethod<jint>(
		"com/obsproject/studio/ObsCodecCapabilities", "queryEncoderRejectFlags",
		"(Ljava/lang/String;IIDIIIZZ)I", type.object<jstring>(), jint(width), jint(height), fps,
		jint(bitrate), jint(mode), jint(profile), jboolean(p010), jboolean(allowByteBuffer)));
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		rejectFlags = 0;
	}
	if (index == 0) {
		const QJniObject diag = QJniObject::callStaticObjectMethod(
			"com/obsproject/studio/ObsCodecCapabilities", "describeEncoderQuery",
			"(Ljava/lang/String;IIDIIIZZ)Ljava/lang/String;", type.object<jstring>(), jint(width),
			jint(height), fps, jint(bitrate), jint(mode), jint(profile), jboolean(p010),
			jboolean(allowByteBuffer));
		if (env->ExceptionCheck())
			env->ExceptionClear();
		else if (diag.isValid())
			diagnostics = diag.toString().toUtf8();
	}
	if (!list.isValid())
		return false;
	auto array = list.object<jobjectArray>();
	if (index >= uint32_t(env->GetArrayLength(array)))
		return false;
	auto item = static_cast<jstring>(env->GetObjectArrayElement(array, jsize(index)));
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return false;
	}
	if (!item)
		return false;
	QJniObject raw(item);
	env->DeleteLocalRef(item);
	const QStringList parts = raw.toString().split(QLatin1Char('|'));
	if (parts.size() >= 2) {
		entry.tier = parts[0].toUtf8();
		entry.name = parts[1].toUtf8();
		if (parts.size() >= 3)
			entry.mismatch = parts.mid(2).join(QLatin1Char('|')).toUtf8();
	} else {
		entry.tier = "strict";
		entry.name = raw.toString().toUtf8();
	}
	return !entry.name.isEmpty();
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

video_range_type OBSAndroidVideoColorRange()
{
	obs_video_info video;
	return obs_get_video_info(&video) ? video.range : VIDEO_RANGE_PARTIAL;
}

static const char *softwareFallbackEncoderForId(const char *id)
{
	if (!id)
		return nullptr;
	if (strcmp(id, "android_mediacodec_h264") == 0)
		return obs_get_encoder_codec("obs_x264") ? "obs_x264" : nullptr;
	if (strcmp(id, "android_mediacodec_hevc") == 0 || strcmp(id, "android_mediacodec_hevc_main10") == 0)
		return obs_get_encoder_codec("ffmpeg_x265") ? "ffmpeg_x265" : nullptr;
	if (strcmp(id, "android_mediacodec_av1") == 0)
		return obs_get_encoder_codec("ffmpeg_aom_av1") ? "ffmpeg_aom_av1" : nullptr;
	return nullptr;
}

static void androidColorSpaceParams(video_colorspace space, jint &colorStandard, jint &colorTransfer)
{
	switch (space) {
	case VIDEO_CS_2100_PQ:
		colorStandard = 6; // COLOR_STANDARD_BT2020
		colorTransfer = 6; // COLOR_TRANSFER_ST2084
		break;
	case VIDEO_CS_2100_HLG:
		colorStandard = 6; // COLOR_STANDARD_BT2020
		colorTransfer = 7; // COLOR_TRANSFER_HLG
		break;
	case VIDEO_CS_601:
		colorStandard = 4; // COLOR_STANDARD_BT601_NTSC
		colorTransfer = 3; // COLOR_TRANSFER_SDR_VIDEO
		break;
	default:
		colorStandard = 1; // COLOR_STANDARD_BT709
		colorTransfer = 3; // COLOR_TRANSFER_SDR_VIDEO
		break;
	}
}

bool OBSAndroidHardwareFullRangeSupported(const char *id, video_colorspace space, bool cbr)
{
	if (!id || strncmp(id, "android_mediacodec_", 19) != 0)
		return true;
	const char *codec = obs_get_encoder_codec(id);
	if (!codec)
		return false;
	const char *mime = nullptr;
	jint profile = 0;
	bool p010 = false;
	if (strcmp(codec, "h264") == 0) {
		mime = "video/avc";
	} else if (strcmp(id, "android_mediacodec_hevc_main10") == 0) {
		mime = "video/hevc";
		p010 = true;
		profile = obs_android_hevc_main10_profile(space == VIDEO_CS_2100_PQ);
	} else if (strcmp(codec, "hevc") == 0) {
		mime = "video/hevc";
		profile = 1;
	} else if (strcmp(codec, "av1") == 0) {
		mime = "video/av01";
		profile = 1;
	} else if (strcmp(codec, "vp9") == 0) {
		mime = "video/x-vnd.on2.vp9";
		profile = 1;
	} else {
		return false;
	}
	jint colorStandard = 1;
	jint colorTransfer = 3;
	androidColorSpaceParams(space, colorStandard, colorTransfer);
	QJniEnvironment env;
	if (!env.isValid())
		return false;
	const auto jmime = QJniObject::fromString(QString::fromUtf8(mime));
	const jboolean ok = QJniObject::callStaticMethod<jboolean>(
		"com/obsproject/studio/ObsCodecCapabilities", "hasHardwareFullRangeSupport",
		"(Ljava/lang/String;IZIIZ)Z", jmime.object<jstring>(), profile, jboolean(p010), colorStandard,
		colorTransfer, jboolean(cbr));
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return false;
	}
	return ok;
}

static const char *resolveAndroidEncoder(const char *id, video_colorspace space, video_range_type range, bool cbr,
					     OBSAndroidSoftwareFallbackReason *reasonOut)
{
	if (reasonOut)
		*reasonOut = OBSAndroidSoftwareFallbackReason::None;
	if (!id || strncmp(id, "android_mediacodec_", 19) != 0)
		return id;

	const char *resolved = id;
	constexpr auto hevcMain10 = "android_mediacodec_hevc_main10";
	const bool pq = space == VIDEO_CS_2100_PQ;
	const bool hlg = space == VIDEO_CS_2100_HLG;
	if (strcmp(id, "android_mediacodec_hevc") == 0 || strcmp(id, hevcMain10) == 0) {
		if (pq || hlg) {
			if (OBSAndroidHevcMain10Available(pq, cbr)) {
				resolved = hevcMain10;
			} else if (obs_get_encoder_codec("ffmpeg_x265")) {
				if (reasonOut)
					*reasonOut = OBSAndroidSoftwareFallbackReason::HdrMain10Unavailable;
				return "ffmpeg_x265";
			}
		}
	}

	if (range == VIDEO_RANGE_FULL && !OBSAndroidHardwareFullRangeSupported(resolved, space, cbr)) {
		const char *sw = softwareFallbackEncoderForId(resolved);
		if (sw) {
			if (reasonOut)
				*reasonOut = OBSAndroidSoftwareFallbackReason::FullRangeHardwareUnavailable;
			return sw;
		}
	}
	return resolved;
}

OBSAndroidSoftwareFallbackReason OBSAndroidSoftwareFallbackForVideo(const char *id, video_colorspace space,
								    video_range_type range, bool cbr)
{
	OBSAndroidSoftwareFallbackReason reason = OBSAndroidSoftwareFallbackReason::None;
	resolveAndroidEncoder(id, space, range, cbr, &reason);
	return reason;
}

bool OBSAndroidEncoderUsesSoftwareFallback(const char *id, video_colorspace space, bool cbr)
{
	return OBSAndroidSoftwareFallbackForVideo(id, space, OBSAndroidVideoColorRange(), cbr) !=
	       OBSAndroidSoftwareFallbackReason::None;
}

const char *OBSAndroidEncoderForVideo(const char *id, bool cbr, bool *fellBackToSoftware,
				      OBSAndroidSoftwareFallbackReason *reasonOut)
{
	OBSAndroidSoftwareFallbackReason reason = OBSAndroidSoftwareFallbackReason::None;
	const auto space = OBSAndroidVideoColorSpace();
	const auto range = OBSAndroidVideoColorRange();
	const char *resolved = resolveAndroidEncoder(id, space, range, cbr, &reason);
	if (reason == OBSAndroidSoftwareFallbackReason::HdrMain10Unavailable) {
		blog(LOG_WARNING,
		     "MediaCodec HEVC Main10 unavailable for the active HDR color space. Using %s.", resolved);
	} else if (reason == OBSAndroidSoftwareFallbackReason::FullRangeHardwareUnavailable) {
		blog(LOG_WARNING,
		     "MediaCodec hardware encoder %s does not support Full color range; falling back to %s.",
		     id ? id : "(null)", resolved);
	}
	if (fellBackToSoftware)
		*fellBackToSoftware = reason != OBSAndroidSoftwareFallbackReason::None;
	if (reasonOut)
		*reasonOut = reason;
	return resolved;
}

static void queryEncoderAvailability(void *, calldata_t *cd)
{
	calldata_set_bool(cd, "supported", hasHardwareEncoder(calldata_string(cd, "mime"), -1,
							     jint(calldata_int(cd, "profile")), calldata_bool(cd, "p010")));
}

static void queryCodecInternal(calldata_t *cd, bool allowByteBuffer)
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
	CodecCandidateEntry entry;
	int rejectFlags = 0;
	QByteArray diagnostics;
	if (width > 0 && height > 0 && num > 0 && den > 0 &&
	    width <= INT32_MAX && height <= INT32_MAX && num <= INT32_MAX && den <= INT32_MAX &&
	    index >= 0 && index <= INT32_MAX && bitrate >= 0 && bitrate <= INT32_MAX &&
	    profile >= 0 && profile <= INT32_MAX && mode >= 0 && mode <= 2 && mime)
		findCodecCandidate(mime, uint32_t(width), uint32_t(height), uint32_t(num), uint32_t(den),
				   uint32_t(index), uint32_t(bitrate), int(mode), int(profile), p010, allowByteBuffer,
				   entry, rejectFlags, diagnostics);
	const bool relaxed = !entry.tier.isEmpty() && entry.tier != "strict";
	const bool relaxedMode = entry.tier == "relaxed_mode";
	calldata_set_string(cd, "name", entry.name.constData());
	calldata_set_bool(cd, "relaxed", relaxed);
	calldata_set_bool(cd, "relaxed_mode", relaxedMode);
	calldata_set_string(cd, "mismatch", entry.mismatch.constData());
	calldata_set_int(cd, "reject_flags", rejectFlags);
	if (entry.name.isEmpty()) {
		blog(LOG_INFO,
		     "MediaCodec %s %lldx%lld %lld/%lld bitrate=%lld mode=%lld(%s) profile=%lld p010=%d index=%lld raw=%d: end of compatible hardware candidates (reject_flags=0x%x)",
		     mime ? mime : "unknown", width, height, num, den, bitrate, mode, rateModeLabel(mode), profile,
		     p010 ? 1 : 0, index, allowByteBuffer ? 1 : 0, rejectFlags);
	} else if (relaxed) {
		blog(LOG_WARNING,
		     "MediaCodec %s %lldx%lld %lld/%lld bitrate=%lld mode=%lld(%s) profile=%lld p010=%d index=%lld raw=%d: %s (tier=%s mismatch=%s)",
		     mime ? mime : "unknown", width, height, num, den, bitrate, mode, rateModeLabel(mode), profile,
		     p010 ? 1 : 0, index, allowByteBuffer ? 1 : 0, entry.name.constData(), entry.tier.constData(),
		     entry.mismatch.isEmpty() ? "none" : entry.mismatch.constData());
	} else {
		blog(LOG_INFO,
		     "MediaCodec %s %lldx%lld %lld/%lld bitrate=%lld mode=%lld(%s) profile=%lld p010=%d index=%lld raw=%d: %s (tier=strict)",
		     mime ? mime : "unknown", width, height, num, den, bitrate, mode, rateModeLabel(mode), profile,
		     p010 ? 1 : 0, index, allowByteBuffer ? 1 : 0, entry.name.constData());
	}
	if (index == 0 && !diagnostics.isEmpty() && (entry.name.isEmpty() || relaxed)) {
		blog(LOG_WARNING, "MediaCodec capabilities (%s %lldx%lld %lld/%lld mode=%s raw=%d): %s",
		     mime ? mime : "unknown", width, height, num, den, rateModeLabel(mode), allowByteBuffer ? 1 : 0,
		     diagnostics.constData());
	}
}

static void queryCodec(void *, calldata_t *cd)
{
	queryCodecInternal(cd, false);
}

static void queryRawCodec(void *, calldata_t *cd)
{
	queryCodecInternal(cd, true);
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
		"void android_mediacodec_query_component(string mime, int width, int height, int fps_num, int fps_den, int bitrate, int mode, int index, int profile, bool p010, out string name, out bool relaxed, out bool relaxed_mode, out string mismatch, out int reject_flags)",
		queryCodec, nullptr);
	proc_handler_add(obs_get_proc_handler(),
		"void android_mediacodec_query_raw_component(string mime, int width, int height, int fps_num, int fps_den, int bitrate, int mode, int index, int profile, bool p010, out string name, out bool relaxed, out bool relaxed_mode, out string mismatch, out int reject_flags)",
		queryRawCodec, nullptr);
}
