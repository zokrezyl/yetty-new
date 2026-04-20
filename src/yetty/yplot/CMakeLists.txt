add_library(yetty_yplot STATIC
    yplot.c
)

target_include_directories(yetty_yplot PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
    ${INCBIN_INCLUDE_DIR}
)

set_source_files_properties(yplot.c PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/yplot.wgsl"
)

target_compile_definitions(yetty_yplot PRIVATE
    YETTY_YPLOT_SHADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/yplot.wgsl"
)

target_link_libraries(yetty_yplot PUBLIC
    yetty_ypaint_core
    yetty_yrender
    yetty_yfsvm
)
