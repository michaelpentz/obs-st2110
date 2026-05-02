# OBS Studio source plugin for SMPTE ST 2110-20 receive.
#
# Two build paths:
#
# 1. Manual paths (Windows, what build-windows.bat uses):
#      cmake -DBUILD_OBS_PLUGIN=ON \
#            -DOBS_SOURCE_DIR=<obs-source-dir> \
#            -DOBS_LIB=path/to/obs.lib
#
# 2. find_package (Linux with libobs-dev installed):
#      cmake -DBUILD_OBS_PLUGIN=ON
#
# See obs-plugin/BUILDING.md for the full recipe.

cmake_minimum_required(VERSION 3.16)

if(OBS_SOURCE_DIR AND OBS_LIB)
    # Manual mode: caller pointed us at headers + import lib.
    if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs-module.h")
        message(FATAL_ERROR "OBS_SOURCE_DIR=${OBS_SOURCE_DIR} does not contain libobs/obs-module.h")
    endif()
    if(NOT EXISTS "${OBS_LIB}")
        message(FATAL_ERROR "OBS_LIB=${OBS_LIB} not found")
    endif()
    set(_obs_includes "${OBS_SOURCE_DIR}/libobs")
    set(_obs_libs "${OBS_LIB}")
    set(_obs_have_target FALSE)
else()
    find_package(libobs QUIET)
    if(NOT libobs_FOUND)
        message(WARNING
            "libobs not found and OBS_SOURCE_DIR / OBS_LIB not set. "
            "Skipping obs-plugin build.")
        return()
    endif()
    set(_obs_have_target TRUE)
endif()

add_library(obs-st2110 MODULE
    src/obs-st2110-plugin.c
    src/obs-st2110-source.c
)

target_include_directories(obs-st2110 PRIVATE src)

if(_obs_have_target)
    target_link_libraries(obs-st2110 PRIVATE st2110rx_static OBS::libobs)
else()
    target_include_directories(obs-st2110 PRIVATE ${_obs_includes})
    target_link_libraries(obs-st2110 PRIVATE st2110rx_static ${_obs_libs})
endif()

if(WIN32)
    target_compile_definitions(obs-st2110 PRIVATE _CRT_SECURE_NO_WARNINGS)
    set_target_properties(obs-st2110 PROPERTIES PREFIX "")
endif()

# Install layout follows OBS plugin convention:
#   <obs-plugins>/64bit/obs-st2110.dll
#   <obs-plugins-data>/obs-st2110/locale/en-US.ini
install(TARGETS obs-st2110
    RUNTIME DESTINATION obs-plugins/64bit
    LIBRARY DESTINATION obs-plugins/64bit)
install(DIRECTORY data/
    DESTINATION data/obs-plugins/obs-st2110)
