# nlohmann/json 3.11.3 —— vendored 单头发行版。
#
# 为什么要把这一份放进树里（而不是 find_package）：
#   frontend/CMakeLists.txt:73 无条件 include feature-plugin-manager.cmake，它第一行就是
#   find_package(nlohmann_json 3.11 REQUIRED)；ui-utility.cmake 的 GoLiveAPI_*.cpp 与
#   MultitrackVideoOutput.cpp 也都要完整的 nlohmann/json.hpp。而 nlohmann 不在 deps-android
#   的清单里（那份清单是给 libobs/插件的），所以：
#     - Windows 开发机靠"机器上恰好装了 conda 的 nlohmann_json"撞过去；
#     - 裸 CI runner 上则一路编到 frontend 第 345 个目标才因为
#       'nlohmann/json_fwd.hpp' file not found 失败。
#   两条路都不可复现。把 3.11.3 的单头版本放进 deps/ 之后，构建不再依赖宿主环境。
#
# 只提供 INTERFACE 目标，与官方 nlohmann_json::nlohmann_json 同名同语义，
# 这样 frontend 的 target_link_libraries 不需要改。
#
# 升级方式：把 single_include/nlohmann/{json.hpp,json_fwd.hpp} 换成新版本，
# 同步下面的版本号，并更新 LICENSE.MIT。json_fwd.hpp 必须一起换 ——
# GoLiveAPI_CensoredJson.hpp 只 include 它，缺了就是 2026-02 CI 那次失败的原因。

add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)

target_include_directories(
  nlohmann_json
  INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/single_include"
)

set(nlohmann_json_VERSION "3.11.3")
set(nlohmann_json_FOUND TRUE)