add_library(yetty_webgpu
    error.c
)

target_include_directories(yetty_webgpu PUBLIC
    ${YETTY_ROOT}/include
)

target_link_libraries(yetty_webgpu
    webgpu
)
