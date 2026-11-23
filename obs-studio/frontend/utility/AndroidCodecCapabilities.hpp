#pragma once
#include <cstdint>
#include <media-io/video-io.h>

// Returns false on JNI failure or when no hardware AVC encoder meets all requirements.
bool OBSAndroidAvcAvailable(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den);
bool OBSAndroidCbrEncoderAvailable(const char *id);
bool OBSAndroidHevcMain10Available(bool pq, bool cbr);
bool OBSAndroidEncoderUsesSoftwareFallback(const char *id, video_colorspace space, bool cbr);
video_colorspace OBSAndroidVideoColorSpace();
const char *OBSAndroidEncoderForVideo(const char *id, bool cbr = false, bool *fellBackToSoftware = nullptr);
void OBSAndroidRegisterCodecQuery();
