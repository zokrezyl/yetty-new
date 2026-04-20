add_library(yetty_yfsvm STATIC
    compiler.c
)

target_include_directories(yetty_yfsvm PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(yetty_yfsvm PUBLIC
    yetty_yexpr
    yetty_yrender
)

# Embed shader via incbin
incbin_add_resources(yetty_yfsvm
    yfsvm_shader "${CMAKE_CURRENT_SOURCE_DIR}/yfsvm.gen.wgsl"
)
