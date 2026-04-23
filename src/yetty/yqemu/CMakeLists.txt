# yqemu - QEMU launcher

add_library(yetty_qemu STATIC
    qemu.c
)

target_include_directories(yetty_qemu
    PUBLIC ${YETTY_ROOT}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(yetty_qemu
    PRIVATE yetty_ycore
)
