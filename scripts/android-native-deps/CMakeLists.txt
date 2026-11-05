# SPDX-License-Identifier: GPL-2.0-or-later
# Windows NDK CMake wrappers for libraries whose upstream build is autotools.
# Source lists follow each project's Android.mk or Makefile.am.
cmake_minimum_required(VERSION 3.22)
project(obs_android_native_deps C)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT DEFINED DEPS_SOURCE_ROOT)
  message(FATAL_ERROR "DEPS_SOURCE_ROOT must point at downloaded upstream sources")
endif()
set(USB "${DEPS_SOURCE_ROOT}/libusb-1.0.28")
set(SPX "${DEPS_SOURCE_ROOT}/speexdsp-1.2.1")
set(RNN "${DEPS_SOURCE_ROOT}/rnnoise-master")
add_library(usb-1.0 STATIC
  ${USB}/libusb/core.c ${USB}/libusb/descriptor.c ${USB}/libusb/hotplug.c
  ${USB}/libusb/io.c ${USB}/libusb/sync.c ${USB}/libusb/strerror.c
  ${USB}/libusb/os/linux_usbfs.c ${USB}/libusb/os/events_posix.c
  ${USB}/libusb/os/threads_posix.c ${USB}/libusb/os/linux_netlink.c)
target_include_directories(usb-1.0 PRIVATE ${USB}/android ${USB}/libusb ${USB}/libusb/os)
target_compile_options(usb-1.0 PRIVATE -fvisibility=hidden)
install(TARGETS usb-1.0 ARCHIVE DESTINATION lib)
install(FILES ${USB}/libusb/libusb.h DESTINATION include/libusb-1.0)

# Equivalent to configure's standard integer type substitutions on Android.
set(INCLUDE_STDINT "#include <stdint.h>")
set(SIZE16 int16_t)
set(USIZE16 uint16_t)
set(SIZE32 int32_t)
set(USIZE32 uint32_t)
configure_file(${SPX}/include/speex/speexdsp_config_types.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/include/speex/speexdsp_config_types.h @ONLY)
add_library(speexdsp STATIC
  ${SPX}/libspeexdsp/preprocess.c ${SPX}/libspeexdsp/jitter.c ${SPX}/libspeexdsp/mdf.c
  ${SPX}/libspeexdsp/fftwrap.c ${SPX}/libspeexdsp/filterbank.c ${SPX}/libspeexdsp/resample.c
  ${SPX}/libspeexdsp/buffer.c ${SPX}/libspeexdsp/scal.c ${SPX}/libspeexdsp/kiss_fft.c
  ${SPX}/libspeexdsp/kiss_fftr.c)
target_include_directories(speexdsp PRIVATE ${SPX}/include ${CMAKE_CURRENT_BINARY_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/include/speex)
target_compile_definitions(speexdsp PRIVATE FLOATING_POINT USE_KISS_FFT EXPORT=)
install(TARGETS speexdsp ARCHIVE DESTINATION lib)
install(DIRECTORY ${SPX}/include/speex/ DESTINATION include/speex FILES_MATCHING PATTERN "*.h")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/speex/speexdsp_config_types.h DESTINATION include/speex)

add_library(rnnoise STATIC ${RNN}/src/denoise.c ${RNN}/src/rnn.c ${RNN}/src/rnn_data.c
  ${RNN}/src/rnn_reader.c ${RNN}/src/pitch.c ${RNN}/src/kiss_fft.c ${RNN}/src/celt_lpc.c)
target_include_directories(rnnoise PRIVATE ${RNN}/include ${RNN}/src)
target_compile_definitions(rnnoise PRIVATE RNNOISE_BUILD)
install(TARGETS rnnoise ARCHIVE DESTINATION lib)
install(FILES ${RNN}/include/rnnoise.h DESTINATION include)
