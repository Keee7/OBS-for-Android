# Build the patched QPA plugin against the same Qt used by androiddeployqt.
include(FetchContent)
find_package(Qt6 6.11.1 EXACT CONFIG REQUIRED COMPONENTS CorePrivate GuiPrivate)
FetchContent_Declare(
  obs_qtbase_android
  URL https://codeload.github.com/qt/qtbase/tar.gz/refs/tags/v6.11.1
  URL_HASH SHA256=e20852bd45cdef5da5175f3634e285e03e2be7ca437f3d2b7e1a2af7321bca7a
  SOURCE_SUBDIR obs-no-cmake
)

# Reuse Qt's source list and conditions without depending on QtBuildInternals.
function(qt_find_package name)
  if(name STREQUAL "EGL" AND NOT TARGET EGL::EGL)
    add_library(EGL::EGL INTERFACE IMPORTED)
    target_link_libraries(EGL::EGL INTERFACE EGL)
  endif()
endfunction()

function(qt_internal_add_plugin target)
  cmake_parse_arguments(PARSE_ARGV 1 plugin "" "OUTPUT_NAME;PLUGIN_TYPE;DEFAULT_IF"
    "SOURCES;NO_UNITY_BUILD_SOURCES;INCLUDE_DIRECTORIES;LIBRARIES")
  add_library(${target} MODULE ${plugin_SOURCES})
  target_include_directories(${target} PRIVATE ${plugin_INCLUDE_DIRECTORIES})
  target_compile_options(${target} PRIVATE -Wno-error=cpp)
  target_link_libraries(${target} PRIVATE ${plugin_LIBRARIES} log)
  set_target_properties(
    ${target}
    PROPERTIES
      AUTOMOC ON
      UNITY_BUILD OFF
      OUTPUT_NAME "plugins_platforms_qtforandroid_${ANDROID_ABI}"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qt-android-platform"
  )
endfunction()

function(qt_internal_extend_target target)
  cmake_parse_arguments(PARSE_ARGV 1 plugin "" "" "CONDITION;SOURCES;NO_UNITY_BUILD_SOURCES;LIBRARIES")
  if(${plugin_CONDITION})
    target_sources(${target} PRIVATE ${plugin_SOURCES})
    if(plugin_LIBRARIES)
      target_link_libraries(${target} PRIVATE ${plugin_LIBRARIES})
    endif()
  endif()
endfunction()

FetchContent_MakeAvailable(obs_qtbase_android)
set(QtBase_SOURCE_DIR "${obs_qtbase_android_SOURCE_DIR}")

set(patch "${CMAKE_CURRENT_LIST_DIR}/qt-6.11.1-accessibility.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
file(SHA256 "${patch}" patch_hash)
string(SUBSTRING "${patch_hash}" 0 16 patch_hash)
set(patched_source "${CMAKE_CURRENT_BINARY_DIR}/qtbase-${patch_hash}")
if(NOT EXISTS "${patched_source}/applied")
  file(COPY "${QtBase_SOURCE_DIR}/src/plugins/platforms/android" DESTINATION "${patched_source}/src/plugins/platforms")
  execute_process(
    COMMAND git -c core.longpaths=true apply "${patch}"
    WORKING_DIRECTORY "${patched_source}"
    COMMAND_ERROR_IS_FATAL ANY
  )
  file(TOUCH "${patched_source}/applied")
endif()
set(QT_FEATURE_android_style_assets ON)
add_subdirectory("${patched_source}/src/plugins/platforms/android" "${CMAKE_CURRENT_BINARY_DIR}/qpa")

# The built-in Qt plugin remains the SDK's deployment source. Packaging
# replaces only its staged copy after androiddeployqt finishes.
add_dependencies(obs-studio QAndroidIntegrationPlugin)
