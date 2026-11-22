# OBS CMake Android platform module
#
# bionic 就是一个 POSIX libm/pthread 实现，所以 libobs 的平台层几乎全部复用 Linux 文件，
# 只有两处必须替换：
#   * obs-nix.c        -> obs-android.c       （模块/资源路径、系统信息、热键）
#   * os_generate_uuid -> platform-android.c  （NDK 没有 libuuid）
# 另外有一处是"原样复用、不替换"：
#   * obs-nix-platform.c —— 只是一个 static enum + 一个 display 指针，零 X11/Wayland 依赖。
#     Linux 上这个文件由 os-linux.cmake 加入，前端的 OBSBasic.cpp(isWayland) 和
#     importers/studio.cpp 都要 obs_get_nix_platform()，缺了它前端链接直接报
#     undefined symbol（pass-7 实测）。Android 侧的取值见该文件里的 __ANDROID__ 默认值。
# 音频监听使用基于 Android AAudio 回调流与 AudioManager 输出枚举的后端。

target_sources(
  libobs
  PRIVATE
    obs-android.c
    obs-nix-platform.c
    audio-monitoring/aaudio/aaudio-enum-devices.c
    audio-monitoring/aaudio/aaudio-monitoring-available.c
    audio-monitoring/aaudio/aaudio-output.c
    util/pipe-posix.c
    util/platform-android.c
    util/platform-nix.c
    util/threading-posix.c
    util/threading-posix.h
)

target_compile_definitions(libobs PRIVATE OBS_INSTALL_PREFIX="${OBS_INSTALL_PREFIX}")

# mediandk：FFmpeg --enable-mediacodec 引入 AMediaCodec*/AMediaFormat*，其 NDK stub 从 API 29 起才有，
# 这也是整个 Android 侧以 android-29 为目标（minSdk 29）的原因。aaudio 自 API 26 起由系统提供。
target_link_libraries(libobs PRIVATE ${CMAKE_DL_LIBS} m log android mediandk aaudio)

# 静态 FFmpeg 的汇编用 adrp+add 直接引用 ff_* 数据表；这些符号一旦进入 .dynsym 就成了可抢占符号，
# lld 会报 "relocation R_AARCH64_ADR_PREL_PG_HI21 cannot be used against symbol"。
# 让静态库符号只留在 .symtab、不进动态符号表即可解决，顺带收窄 libobs.so 的导出面。
target_link_options(libobs PRIVATE -Wl,--exclude-libs,ALL)
