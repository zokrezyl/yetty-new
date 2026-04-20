add_library(yetty_yrender
    types.c
    gpu-allocator.c
    gpu-resource-binder.c
    render-target-surface.c
    rendered-layer.c
    layer-renderer-surface.c
)

target_include_directories(yetty_yrender PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${INCBIN_INCLUDE_DIR}
)

target_compile_definitions(yetty_yrender PRIVATE
    BLEND_SHADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/blend.wgsl"
)

target_link_libraries(yetty_yrender
    webgpu
)
