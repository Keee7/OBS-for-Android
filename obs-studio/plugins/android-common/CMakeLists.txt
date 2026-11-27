# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.28...3.30)

add_library(android-common INTERFACE)
add_library(OBS::android-common ALIAS android-common)

target_include_directories(android-common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
