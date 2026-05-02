cmake_minimum_required(VERSION 3.16)
project(obs-st2110 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(libst2110rx)

option(BUILD_OBS_PLUGIN "Build OBS Studio plugin (requires libobs SDK)" OFF)
if(BUILD_OBS_PLUGIN)
    add_subdirectory(obs-plugin)
endif()

# On Windows: BUILD_OBS_PLUGIN=ON + libobs_DIR=<obs-studio-build>/libobs
# On Linux: BUILD_OBS_PLUGIN=ON after `apt install libobs-dev`
# See obs-plugin/BUILDING.md for the full recipe.
