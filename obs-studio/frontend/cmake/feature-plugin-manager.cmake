# deps/nlohmann 提供 vendored 目标（见 frontend/CMakeLists.txt 顶部）；有就不再找宿主包。
if(NOT TARGET nlohmann_json::nlohmann_json)
  find_package(nlohmann_json 3.11 REQUIRED)
endif()

set(OBS_PLATFORM_INSTALL_PATH "${CMAKE_INSTALL_PREFIX}/")
set(OBS_PLATFORM_LIBRARY_PATH "${CMAKE_INSTALL_LIBDIR}/")
set(OBS_PLATFORM_DATA_PATH "${CMAKE_INSTALL_DATAROOTDIR}/")
set(OBS_PLATFORM_CORE_MODULE_PATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/obs-modules/core")
set(OBS_PLATFORM_CORE_DATA_PATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-modules/core")
set(OBS_PLATFORM_PLUGIN_MODULE_PATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/obs-modules/plugins")
set(OBS_PLATFORM_PLUGIN_DATA_PATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-modules/plugins")

target_sources(
  obs-studio
  PRIVATE
    plugin-manager/InstalledPluginRow.cpp
    plugin-manager/InstalledPluginRow.hpp
    plugin-manager/PluginManager.cpp
    plugin-manager/PluginManager.hpp
    plugin-manager/PluginManagerWindow.cpp
    plugin-manager/PluginManagerWindow.hpp
    plugin-manager/PluginModuleLoader.hpp
)

if(OS_WINDOWS)
  target_sources(obs-studio PRIVATE plugin-manager/PluginModuleLoader_Windows.cpp)
elseif(OS_MACOS)
  target_sources(obs-studio PRIVATE plugin-manager/PluginModuleLoader_MacOS.mm)
elseif(OS_FLATPAK)
  configure_file(plugin-manager/LoaderPaths_Flatpak.hpp.in LoaderPaths_Flatpak.hpp @ONLY)
  target_sources(obs-studio PRIVATE plugin-manager/PluginModuleLoader_Flatpak.cpp LoaderPaths_Flatpak.hpp)
elseif(OS_LINUX)
  configure_file(plugin-manager/LoaderPaths_Linux.hpp.in LoaderPaths_Linux.hpp @ONLY)
  target_sources(obs-studio PRIVATE plugin-manager/PluginModuleLoader_Linux.cpp LoaderPaths_Linux.hpp)
elseif(OS_FREEBSD OR OS_OPENBSD)
  configure_file(plugin-manager/LoaderPaths_BSD.hpp.in LoaderPaths_BSD.hpp @ONLY)
  target_sources(obs-studio PRIVATE plugin-manager/PluginModuleLoader_BSD.cpp LoaderPaths_BSD.hpp)
elseif(OS_ANDROID)
  # Android：APK 内插件无法按路径扫描，loader 按白名单裸名 dlopen（见其头注释）
  target_sources(obs-studio PRIVATE plugin-manager/PluginModuleLoader_Android.cpp)
endif()

target_link_libraries(obs-studio PRIVATE nlohmann_json::nlohmann_json)
