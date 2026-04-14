add_library(st2110rx_static STATIC
    src/rx_core.c
    src/rx_socket.c
    src/rx_rfc4175.c
    src/rx_convert.c
    src/rx_stats.c
)

target_include_directories(st2110rx_static
    PUBLIC include
    PRIVATE src
)

if(WIN32)
    target_link_libraries(st2110rx_static PRIVATE ws2_32)
endif()

add_executable(st2110rx-cli tools/st2110rx-cli.c)
target_link_libraries(st2110rx-cli PRIVATE st2110rx_static)

add_executable(st2110tx-test tools/st2110tx-test.c)
if(WIN32)
    target_link_libraries(st2110tx-test PRIVATE ws2_32)
endif()
