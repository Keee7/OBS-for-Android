#pragma once
#include <cstdint>
#include <media-io/video-io.h>

enum class OBSAndroidSoftwareFallbackReason {
	None = 0,
	HdrMain10Unavailable,
	FullRangeHardwareUnavailable,
};

// Returns false on JNI failure or when no hardware AVC encoder meets all requirements.
bool OBSAndroidAvcAvailable(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den);
bool OBSAndroidCbrEncoderAvailable(const char *id);
bool OBSAndroidHevcMain10Available(bool pq, bool cbr);
bool OBSAndroidHardwareFullRangeSupported(const char *id, video_colorspace space = VIDEO_CS_709);
OBSAndroidSoftwareFallbackReason OBSAndroidSoftwareFallbackForVideo(const char *id, video_colorspace space,
								    video_range_type range, bool cbr);
bool OBSAndroidEncoderUsesSoftwareFallback(const char *id, video_colorspace space, bool cbr);
video_colorspace OBSAndroidVideoColorSpace();
video_range_type OBSAndroidVideoColorRange();
const char *OBSAndroidEncoderForVideo(const char *id, bool cbr = false, bool *fellBackToSoftware = nullptr,
				      OBSAndroidSoftwareFallbackReason *reasonOut = nullptr);
void OBSAndroidRegisterCodecQuery();
