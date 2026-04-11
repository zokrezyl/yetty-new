add_library(yetty_term
    terminal.c
    text-layer.c
    ypaint-layer.c
    pty-reader.c
    osc-args.c
)

target_include_directories(yetty_term PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${INCBIN_INCLUDE_DIR}
)

target_compile_definitions(yetty_term PRIVATE
    TEXT_LAYER_SHADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/text-layer.wgsl"
    YPAINT_LAYER_SHADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ypaint-layer.wgsl"
)

target_link_libraries(yetty_term
    vterm
    webgpu
    ypaint_core
)
