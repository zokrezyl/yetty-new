add_library(ypaint_core STATIC
    buffer.c
)

target_include_directories(ypaint_core PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
)
