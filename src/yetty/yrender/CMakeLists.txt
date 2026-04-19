add_library(yetty_render
    types.c
    gpu-allocator.c
    gpu-resource-binder.c
    render-target-texture.c
)

target_include_directories(yetty_render PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${INCBIN_INCLUDE_DIR}
)

target_compile_definitions(yetty_render PRIVATE
    BLEND_SHADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/blend.wgsl"
)

target_link_libraries(yetty_render
    webgpu
)
