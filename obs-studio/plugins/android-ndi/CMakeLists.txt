cmake_minimum_required(VERSION 3.28...3.30)

add_library(android-ndi MODULE)
add_library(OBS::android-ndi ALIAS android-ndi)

target_sources(
  android-ndi
  PRIVATE
    ndi-abi.h
    ndi-finder.c
    ndi-finder.h
    ndi-output.c
    ndi-output.h
    ndi-runtime.c
    ndi-runtime.h
    ndi-source.c
    ndi-sync.h
)

# Optional: point -DOBS_ANDROID_NDI_SDK_DIR=<path> at an official NDI SDK for Android
# directory to compile against official Processing.NDI.Lib.h headers. Runtime still
# loads libndi.so dynamically via dlopen/dlsym so normal CI builds need no proprietary SDK.
set(OBS_ANDROID_NDI_SDK_DIR "" CACHE PATH "Optional path to official NDI SDK for Android")
if(OBS_ANDROID_NDI_SDK_DIR AND EXISTS "${OBS_ANDROID_NDI_SDK_DIR}/include/Processing.NDI.Lib.h")
  target_include_directories(android-ndi PRIVATE "${OBS_ANDROID_NDI_SDK_DIR}/include")
  target_compile_definitions(android-ndi PRIVATE OBS_ANDROID_NDI_HAVE_SDK_HEADERS=1)
  message(STATUS "android-ndi: using official NDI headers from ${OBS_ANDROID_NDI_SDK_DIR}/include")
endif()

target_link_libraries(android-ndi PRIVATE OBS::libobs dl $<$<PLATFORM_ID:Android>:log>)
set_target_properties_obs(android-ndi PROPERTIES FOLDER plugins/android-ndi PREFIX "")

if(ANDROID)
  # MediaCodec-backed FFmpeg 7.x ABI bridge for NDI|HX (H.264 / HEVC / AAC) decoding.
  # Packaged into lib/arm64-v8a/ with DT_SONAME matching what libndi.so dynamically loads
  # (libavcodec-ndi.so.61 and libavutil-ndi.so.59).
  add_library(obs_ndi_avutil59 SHARED ndi-hx-avabi7.h ndi-hx-avutil59.c)
  add_library(OBS::obs_ndi_avutil59 ALIAS obs_ndi_avutil59)
  target_link_options(obs_ndi_avutil59 PRIVATE "-Wl,-soname,libavutil-ndi.so.59" "-Wl,-z,max-page-size=16384")
  set_target_properties(obs_ndi_avutil59 PROPERTIES FOLDER plugins/android-ndi C_VISIBILITY_PRESET hidden NO_SONAME ON)

  add_library(obs_ndi_avcodec61 SHARED ndi-hx-avabi7.h ndi-hx-avcodec61.c)
  add_library(OBS::obs_ndi_avcodec61 ALIAS obs_ndi_avcodec61)
  target_link_libraries(obs_ndi_avcodec61 PRIVATE mediandk log m)
  target_link_options(obs_ndi_avcodec61 PRIVATE "-Wl,-soname,libavcodec-ndi.so.61" "-Wl,-z,max-page-size=16384")
  set_target_properties(obs_ndi_avcodec61 PROPERTIES FOLDER plugins/android-ndi C_VISIBILITY_PRESET hidden NO_SONAME ON)
endif()
