add_library(yetty_yplot STATIC
    yplot.c
)

target_include_directories(yetty_yplot PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
)

target_link_libraries(yetty_yplot PUBLIC
    yetty_ypaint_core
    yetty_yrender
    yetty_yfsvm
)

# Embed shader via incbin
incbin_add_resources(yetty_yplot
    yplot_shader "${CMAKE_CURRENT_SOURCE_DIR}/yplot.wgsl"
)
