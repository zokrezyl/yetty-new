add_library(yetty_yfsvm STATIC
    compiler.c
)

target_include_directories(yetty_yfsvm PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${INCBIN_INCLUDE_DIR}
)

set_source_files_properties(compiler.c PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/yfsvm.gen.wgsl"
)

target_compile_definitions(yetty_yfsvm PRIVATE
    YETTY_YFSVM_SHADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/yfsvm.gen.wgsl"
)

target_link_libraries(yetty_yfsvm PUBLIC
    yetty_yexpr
    yetty_yrender
)
