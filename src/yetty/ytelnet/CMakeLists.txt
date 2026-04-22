# ytelnet - Telnet PTY library

add_library(yetty_telnet STATIC
    telnet-pty.c
)

target_include_directories(yetty_telnet
    PUBLIC ${YETTY_ROOT}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(yetty_telnet
    PRIVATE yetty_ycore
)
