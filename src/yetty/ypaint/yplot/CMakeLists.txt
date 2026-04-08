add_library(ypaint_yplot STATIC
    yplot.c
)

target_include_directories(ypaint_yplot PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
)

target_link_libraries(ypaint_yplot PUBLIC ypaint_core)
