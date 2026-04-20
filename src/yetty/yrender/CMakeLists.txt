add_library(yetty_yrender
    types.c
    gpu-allocator.c
    gpu-resource-binder.c
    render-target-texture.c
)

target_include_directories(yetty_yrender PUBLIC
    ${YETTY_ROOT}/include
    ${INCBIN_INCLUDE_DIR}
)

target_compile_definitions(yetty_yrender PRIVATE
    BLEND_SHADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/blend.wgsl"
)

target_link_libraries(yetty_yrender
    webgpu
)

# Embed shaders via incbin (or stubs on Emscripten)
incbin_add_resources(yetty_render
    blend_shader "${CMAKE_CURRENT_SOURCE_DIR}/blend.wgsl"
)
