# deps/nlohmann 提供 vendored 目标（见 frontend/CMakeLists.txt 顶部）；有就不再找宿主包。
if(NOT TARGET nlohmann_json::nlohmann_json)
  find_package(nlohmann_json 3.11 REQUIRED)
endif()

target_sources(
  obs-studio
  PRIVATE
    plugin-manager/PluginManager.cpp
    plugin-manager/PluginManager.hpp
    plugin-manager/PluginManagerWindow.cpp
    plugin-manager/PluginManagerWindow.hpp
)

target_link_libraries(obs-studio PRIVATE nlohmann_json::nlohmann_json)
