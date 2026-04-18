add_library(yetty_webgpu
    error.c
)

target_include_directories(yetty_webgpu PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(yetty_webgpu
    webgpu
)
