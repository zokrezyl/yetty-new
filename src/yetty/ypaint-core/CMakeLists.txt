add_library(yetty_ypaint_core STATIC
    buffer.c
)

target_include_directories(yetty_ypaint_core PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
)

target_link_libraries(yetty_ypaint_core PUBLIC
    yetty_core
)
