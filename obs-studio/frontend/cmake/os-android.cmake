# OBS CMake Android frontend platform module
#
# OBS CMake Android frontend platform module
#
# 对照 os-linux.cmake 的差别全部来自"Android 的 Qt 套件里没有这些东西"：
#   * Qt6DBus      —— 不存在，所以 platform-x11.cpp 那份走 XDG portal 的实现整体换掉，不链 Qt::DBus。
#   * Libpci       —— Android 没有 PCI 总线枚举，system-info 换成读系统属性的实现。
#   * Python       —— Android 上不接脚本，见 cmake/android/defaults.cmake。
#   * metainfo/desktop/图标 install —— FHS 桌面集成产物，Android 用不到。

target_sources(
  obs-studio
  PRIVATE
    utility/AndroidCodecCapabilities.cpp
    utility/AndroidRuntimeBootstrap.cpp
    utility/CrashHandler_Android.cpp
    utility/NativeEventFilter.cpp
    utility/platform-android.cpp
    utility/system-info-android.cpp
    # One Android TextureView and ANativeWindow for each OBS display.
    # 对应的 Java 侧在 cmake/android/java/com/obsproject/studio/ObsDisplayHost.java，
    # androiddeployqt 从 QT_ANDROID_PACKAGE_SOURCE_DIR 自动收 java/ 目录，不需要在这里列。
    widgets/OBSAndroidDisplay.cpp
    # 推流/录制期间的前台服务起停桥（Java 侧 cmake/android/java/.../ObsForegroundService.java）。
    widgets/OBSAndroidKeepAlive.cpp
    # 一期运行时权限申请（Java 侧 .../ObsPermissionHost.java）。
    widgets/OBSAndroidPermissions.cpp
# USB host 总线（obs_android_usb_*）的前端注册（Java 侧 .../ObsUsbHost.java），
# 给 android-capture 的 UVC 源用。
    widgets/OBSAndroidBrowser.cpp
# 音频输入设备总线（obs_android_audio_*）的前端注册（Java 侧 .../ObsAudioHost.java）。
# AAudio 采集源据此列出 USB 声卡等输入。
    widgets/OBSAndroidAudio.cpp
    widgets/OBSAndroidUsb.cpp
    widgets/OBSAndroidNdi.cpp
    # 桌面 "openUrl(本地路径)" 的 Android 替代（显示路径 + 复制剪贴板）。
    widgets/OBSAndroidPath.cpp
)

target_compile_definitions(
  obs-studio
  PRIVATE OBS_INSTALL_PREFIX="${OBS_INSTALL_PREFIX}" $<$<BOOL:${ENABLE_PORTABLE_CONFIG}>:ENABLE_PORTABLE_CONFIG>
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(settings_form "${CMAKE_CURRENT_SOURCE_DIR}/forms/OBSBasicSettings.ui")
set(settings_ui_generator "${CMAKE_CURRENT_LIST_DIR}/android-settings-ui.py")
set(settings_ui_dir "${CMAKE_CURRENT_BINARY_DIR}/android-settings-ui")
set(settings_ui_header "${settings_ui_dir}/android_OBSBasicSettingsUi.hpp")
set_source_files_properties("${settings_form}" PROPERTIES SKIP_AUTOUIC ON)
add_custom_command(
  OUTPUT "${settings_ui_header}"
  COMMAND
    "${Python3_EXECUTABLE}" "${settings_ui_generator}" $<TARGET_FILE:Qt6::uic> "${settings_form}" "${settings_ui_header}"
  DEPENDS "${settings_form}" "${settings_ui_generator}" Qt6::uic
  VERBATIM
)
add_custom_target(obs-android-settings-ui DEPENDS "${settings_ui_header}")
add_dependencies(obs-studio obs-android-settings-ui)
target_include_directories(obs-studio PRIVATE "${settings_ui_dir}")

# 链接期补两类桌面平台上会"自动带上"的依赖：
#
# 1) swresample：frontend/CMakeLists.txt:12 只要了 avcodec/avutil/avformat 三个组件。
#    桌面平台 FFmpeg 是动态库，符号由 libavcodec.so 自己带走；Android 这侧 FFmpeg
#    全是 .a，libavcodec.a 里有 1 处对 swr_convert 的未定义引用，定义在 libswresample.a，
#    所以最终可执行文件必须自己把 swresample 链上。
#    FindFFmpeg 的每个导入目标都有 `NOT TARGET` 保护，重复调用只补建缺的那个。
#
# 2) mediandk / android：libavcodec 是 --enable-mediacodec 构建的，引用 AMediaFormat_*
#    （NDK libmediandk.so，api 29+）和 ANativeWindow_*（libandroid.so）。
#    libobs 那边是在自己的 os-android.cmake 里 PRIVATE 链的，不外泄给前端。
find_package(FFmpeg REQUIRED COMPONENTS swresample)
target_link_libraries(obs-studio PRIVATE FFmpeg::swresample mediandk android)

# 3) 静态 FFmpeg 链进**共享库**：ld.lld 会因 R_X86_64_PC32 重定位拒绝 ——
#    libavcodec.a 里 ff_pb_*/ff_pw_* 常量是**全局可见、可被抢占**的数据符号，链成可执行
#    文件时地址静态、PC 相对寻址合法；链成 `.so` 时链接器不能允许"PC 相对 + 可能被别的
#    DSO 抢占"这种组合（arm64 的 ADR_PREL_PG_HI21 族无此约束，只卡 x86_64）。
#    解法取 `--exclude-libs,ALL`：不把静态归档里的符号导出到 .dynsym ⇒ 符号变成不可抢占，
#    同时仍导出 `main`（Java 侧 dlsym 的前提）。
target_link_options(obs-studio PRIVATE -Wl,--exclude-libs,ALL)

# ---------------------------------------------------------------------------
# 把前端打成 APK
#
# qt_add_executable 在 Android 上建的是 MODULE 库，产物不再是 rundir/bin/obs 那个 ELF
# 可执行文件，而是 frontend/libobs_<abi>.so（SUFFIX 由 qt6_android_apply_arch_suffix
# 设成 _<abi>.so，见 Qt6AndroidMacros.cmake:466-483）。
#
# 资源取**源树**不取 rundir：rundir 里的副本要等对应 ninja target 的 copy 规则才刷新，
# 而 qt_add_resources 的 FILES 是 configure 期定死的清单 —— 新构树上 configure 时 rundir
# 还是空的。
# ---------------------------------------------------------------------------
set(_obs_lib_data "${CMAKE_SOURCE_DIR}/libobs/data")
set(_frontend_data "${CMAKE_CURRENT_SOURCE_DIR}/data")

file(GLOB_RECURSE _obs_lib_data_files CONFIGURE_DEPENDS "${_obs_lib_data}/*")
file(GLOB_RECURSE _frontend_data_files CONFIGURE_DEPENDS "${_frontend_data}/*")
list(LENGTH _obs_lib_data_files _obs_lib_data_count)
list(LENGTH _frontend_data_files _frontend_data_count)
if(_obs_lib_data_count EQUAL 0)
  message(FATAL_ERROR "3-2 打包：${_obs_lib_data} 下没有任何文件 —— APK 里不会有 libobs 数据")
endif()
if(_frontend_data_count EQUAL 0)
  message(FATAL_ERROR "3-2 打包：${_frontend_data} 下没有任何文件 —— APK 里不会有前端 locale/主题")
endif()
message(STATUS "3-2 打包：数据资源 libobs/data=${_obs_lib_data_count} 个文件，frontend/data=${_frontend_data_count} 个文件")

# :/obsdata -> $OBS_ROOT_PATH/share/libobs（obs-android.c 的 find_libobs_data_file）
qt_add_resources(
  obs-studio "obsdata"
  PREFIX "/obsdata"
  BASE "${_obs_lib_data}"
  FILES ${_obs_lib_data_files})

# :/obsfrontenddata -> $OBS_ROOT_PATH/share/obs/obs-studio
# （platform-android.cpp 的 GetDataFilePath 拼的是 <root>/$OBS_DATA_PATH/obs-studio/，
#  OBS_DATA_PATH = share/obs，与桌面安装树同构）
qt_add_resources(
  obs-studio "obsfrontenddata"
  PREFIX "/obsfrontenddata"
  BASE "${_frontend_data}"
  FILES ${_frontend_data_files})

if(EXISTS "${CMAKE_SOURCE_DIR}/AUTHORS")
  qt_add_resources(
    obs-studio "obsauthors"
    PREFIX "/obsfrontenddata/authors"
    BASE "${CMAKE_SOURCE_DIR}"
    FILES "${CMAKE_SOURCE_DIR}/AUTHORS")
endif()

# :/obsplugindata/<模块> -> $OBS_ROOT_PATH/share/obs/obs-plugins/<模块>
#
# 插件数据（.effect / locale / luma_wipes…）没法跟着 .so 一起进 nativeLibraryDir：
# PackageManager 只把 APK 的 lib/<abi>/ 里以 .so 结尾的文件解出来，其余文件连目录都建不出，
# 而 obs_module_file() 最终走 os_file_exists()（真 stat），资源路径 :/... 它不认。
# 所以与 libobs/前端数据同一套路：进 Qt 资源 → 启动时解到应用私有目录。
# 目标清单取全局属性 OBS_MODULES_ENABLED —— 顶层 CMakeLists.txt 里 add_subdirectory(plugins)
# 在 add_subdirectory(frontend) 之前，故此刻已完整；模块名就是 .so 去后缀后的名字，
# 与 libobs/obs-module.c 的 process_found_module 推出的 name 一致，data 目录按同名对上。
# 优先用 plugins/CMakeLists.txt 在过滤 core 清单之前保存的完整列表（可选插件不在
# OBS_MODULES_ENABLED 里，但它们的 data/ 仍要打包）。
get_property(_obs_plugin_modules GLOBAL PROPERTY OBS_ANDROID_ALL_MODULES)
if(NOT _obs_plugin_modules)
  get_property(_obs_plugin_modules GLOBAL PROPERTY OBS_MODULES_ENABLED)
endif()
set(_obs_plugin_data_dirs "")
foreach(_m IN LISTS _obs_plugin_modules)
  get_target_property(_m_src ${_m} SOURCE_DIR)
  if(_m_src AND EXISTS "${_m_src}/data")
    file(GLOB_RECURSE _m_files CONFIGURE_DEPENDS "${_m_src}/data/*")
    list(LENGTH _m_files _m_count)
    if(_m_count GREATER 0)
      string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _m_name ${_m})
      qt_add_resources(
        obs-studio "obsplugindata_${_m_name}"
        PREFIX "/obsplugindata/${_m}"
        BASE "${_m_src}/data"
        FILES ${_m_files})
      list(APPEND _obs_plugin_data_dirs "${_m}(${_m_count})")
    endif()
  endif()
endforeach()
list(JOIN _obs_plugin_data_dirs " " _obs_plugin_data_dirs)
message(STATUS "P-1 打包：插件数据进 APK 资源 = ${_obs_plugin_data_dirs}（候选模块 ${_obs_plugin_modules}）")

# 渲染后端不参与链接（obs_reset_video 时 os_dlopen("libobs-opengl")），所以只有列进
# EXTRA_LIBS 才会被 androiddeployqt 打进 APK 的 lib/<abi>/。文件名本来就以 lib 开头，
# androiddeployqt 要求 EXTRA_LIBS 的文件名以 "lib" 开头，渲染后端符合这个要求。
set(_extra_libs "")
if(TARGET OBS::libobs-opengl)
  add_dependencies(obs-studio OBS::libobs-opengl)
  get_target_property(_gs_dir OBS::libobs-opengl BINARY_DIR)
  set(_gs_backend "${_gs_dir}/libobs-opengl.so")
  list(APPEND _extra_libs "${_gs_backend}")
  message(STATUS "3-2 打包：渲染后端进 APK = ${_gs_backend}")
else()
  message(WARNING "3-2 打包：没有 OBS::libobs-opengl 目标 —— APK 里没有渲染后端，obs_reset_video 会失败")
endif()

foreach(_hx_target OBS::obs_ndi_avutil59 OBS::obs_ndi_avcodec61)
  if(TARGET ${_hx_target})
    add_dependencies(obs-studio ${_hx_target})
    get_target_property(_hx_dir ${_hx_target} BINARY_DIR)
    string(REPLACE "OBS::" "" _hx_name "${_hx_target}")
    list(APPEND _extra_libs "${_hx_dir}/lib${_hx_name}.so")
    message(STATUS "android-ndi: bundling MediaCodec HX bridge lib${_hx_name}.so")
  endif()
endforeach()

# Optional verification-build hook: bundle official arm64-v8a libndi.so when
# -DOBS_ANDROID_NDI_SDK_DIR=<path> is explicitly passed. Default CI leaves it unset.
if(DEFINED OBS_ANDROID_NDI_SDK_DIR AND NOT OBS_ANDROID_NDI_SDK_DIR STREQUAL "")
  foreach(_ndi_cand
          "${OBS_ANDROID_NDI_SDK_DIR}/lib/arm64-v8a/libndi.so"
          "${OBS_ANDROID_NDI_SDK_DIR}/lib/aarch64-linux-android/libndi.so"
          "${OBS_ANDROID_NDI_SDK_DIR}/libndi.so")
    if(EXISTS "${_ndi_cand}")
      list(APPEND _extra_libs "${_ndi_cand}")
      message(STATUS "android-ndi: bundling verification runtime ${_ndi_cand}")
      break()
    endif()
  endforeach()
endif()

# 插件 .so 不进 EXTRA_LIBS（名字不以 lib 开头，androiddeployqt 会要求改名），而是由
# 构建脚本把 rundir/lib/obs-plugins/*.so 暂存到本目录的 libs/<abi>/，
# 走 androiddeployqt 的 android-package-source-directory → gradle jniLibs 那条路进 APK。
# Java/JNI descriptors stay in com.obsproject.studio. The package/application ID
# may be overridden by a platform build without changing the tracked Android sources.
if(NOT DEFINED OBS_ANDROID_PACKAGE_NAME OR OBS_ANDROID_PACKAGE_NAME STREQUAL "")
  set(OBS_ANDROID_PACKAGE_NAME "com.obsproject.studio")
endif()
if(NOT DEFINED OBS_ANDROID_PACKAGE_SOURCE_DIR OR OBS_ANDROID_PACKAGE_SOURCE_DIR STREQUAL "")
  set(OBS_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/android")
endif()
if(NOT DEFINED OBS_ANDROID_COMPILE_SDK_VERSION OR OBS_ANDROID_COMPILE_SDK_VERSION STREQUAL "")
  set(OBS_ANDROID_COMPILE_SDK_VERSION 36)
endif()

set(_obs_android_manifest "${OBS_ANDROID_PACKAGE_SOURCE_DIR}/AndroidManifest.xml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_obs_android_manifest}")
file(READ "${_obs_android_manifest}" _obs_android_manifest_xml)
if(_obs_android_manifest_xml MATCHES "android:versionName=\"([^\"]+)\"")
  set(OBS_ANDROID_VERSION_NAME "${CMAKE_MATCH_1}")
else()
  message(FATAL_ERROR "Cannot parse android:versionName from ${_obs_android_manifest}")
endif()
if(_obs_android_manifest_xml MATCHES "android:versionCode=\"([0-9]+)\"")
  set(OBS_ANDROID_VERSION_CODE "${CMAKE_MATCH_1}")
else()
  message(FATAL_ERROR "Cannot parse android:versionCode from ${_obs_android_manifest}")
endif()
target_compile_definitions(obs-studio PRIVATE OBS_ANDROID_VERSION_NAME="${OBS_ANDROID_VERSION_NAME}")

set_target_properties(
  obs-studio
  PROPERTIES
    QT_ANDROID_EXTRA_LIBS "${_extra_libs}"
    # 清单与 FileProvider 的 paths 都在 cmake/android/ 下自带；一旦设了这个变量，
    # androiddeployqt 就不再拿自己的模板覆盖清单。
    # package 是独立安装用的 application ID；应用代码和 JNI descriptor 继续使用旧 Java 包名。
    QT_ANDROID_PACKAGE_SOURCE_DIR "${OBS_ANDROID_PACKAGE_SOURCE_DIR}"
    QT_ANDROID_PACKAGE_NAME "${OBS_ANDROID_PACKAGE_NAME}"
    # Compress native libraries in the download. Android extracts them at install time.
    QT_ANDROID_LEGACY_PACKAGING TRUE
    # Derived from AndroidManifest.xml above so versionName/versionCode stay in sync.
    QT_ANDROID_VERSION_NAME "${OBS_ANDROID_VERSION_NAME}"
    QT_ANDROID_VERSION_CODE "${OBS_ANDROID_VERSION_CODE}"
    QT_ANDROID_COMPILE_SDK_VERSION "${OBS_ANDROID_COMPILE_SDK_VERSION}"
    QT_ANDROID_TARGET_SDK_VERSION 35
    QT_ANDROID_MIN_SDK_VERSION 29)

add_subdirectory(
  "${CMAKE_CURRENT_LIST_DIR}/../../../scripts/qt-android-ime/android-platform"
  "${CMAKE_CURRENT_BINARY_DIR}/qt-android-platform-build"
)

