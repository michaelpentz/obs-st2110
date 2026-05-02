# OBS Studio source plugin for SMPTE ST 2110-20 receive.
#
# Build requires OBS Studio development headers + import library. On Windows,
# the easiest path is to install OBS Studio (which ships obs.lib + headers in
# the dev package) and point libobs_DIR at its CMake config:
#
#   cmake -B build -G Ninja -DBUILD_OBS_PLUGIN=ON \
#         -Dlibobs_DIR=C:/Program Files/obs-studio/cmake
#
# On Linux, use the system libobs-dev package (apt install libobs-dev).

cmake_minimum_required(VERSION 3.16)

find_package(libobs QUIET)

if(NOT libobs_FOUND)
    message(WARNING
        "libobs not found. Set -Dlibobs_DIR to the directory containing "
        "libobsConfig.cmake. Skipping obs-plugin build.")
    return()
endif()

add_library(obs-st2110 MODULE
    src/obs-st2110-plugin.c
    src/obs-st2110-source.c
)

target_include_directories(obs-st2110 PRIVATE src)
target_link_libraries(obs-st2110 PRIVATE st2110rx_static OBS::libobs)

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
