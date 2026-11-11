#pragma once
#include <cstdint>

// Returns false on JNI failure or when no hardware AVC encoder meets all requirements.
bool OBSAndroidAvcAvailable(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den);
bool OBSAndroidCbrEncoderAvailable(const char *id);
void OBSAndroidRegisterCodecQuery();
