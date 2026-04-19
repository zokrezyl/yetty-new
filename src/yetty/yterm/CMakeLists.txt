add_library(yetty_term
    terminal.c
    text-layer.c
    ypaint-layer.c
    pty-reader.c
    osc-args.c
)

target_include_directories(yetty_term PUBLIC
    ${YETTY_ROOT}/include
)

target_link_libraries(yetty_term
    vterm
    webgpu
    yetty_ypaint_core
    yetty_ypaint_yaml
    yetty_yfont
    yetty_yui
)
