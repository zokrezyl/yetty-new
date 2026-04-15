# yetty font subsystem (C)
add_library(yetty_yfont STATIC
    ms-raster-font.c
    ms-msdf-font.c
)

target_include_directories(yetty_yfont PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
    ${FREETYPE_INCLUDE_DIR}
    ${INCBIN_INCLUDE_DIR}
)

target_link_libraries(yetty_yfont PUBLIC ${FREETYPE_ALL_LIBS} webgpu yetty_ycdb)

target_compile_definitions(yetty_yfont PRIVATE
    YETTY_YFONT_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
