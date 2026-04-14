cmake_minimum_required(VERSION 3.16)
project(obs-st2110 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(libst2110rx)

option(BUILD_OBS_PLUGIN "Build OBS Studio plugin" OFF)
if(BUILD_OBS_PLUGIN)
    add_subdirectory(obs-plugin)
endif()
