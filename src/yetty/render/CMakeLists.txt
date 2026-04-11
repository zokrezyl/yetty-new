add_library(yetty_render
    types.c
    gpu-allocator.c
    gpu-resource-binder.c
)

target_include_directories(yetty_render PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(yetty_render
    webgpu
)
