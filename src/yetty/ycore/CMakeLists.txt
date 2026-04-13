# yetty core (C)
add_library(yetty_core STATIC
    util.c
)

target_include_directories(yetty_core PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
    ${DAWN_INCLUDE_DIR}
)

target_link_libraries(yetty_core PUBLIC
    webgpu
)
