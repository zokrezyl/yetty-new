# yetty font subsystem (C)
add_library(yetty_yfont STATIC
    ms-raster-font.c
    ms-msdf-font.c
    msdf-font.c
)

target_include_directories(yetty_yfont PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
    ${FREETYPE_INCLUDE_DIR}
)

target_link_libraries(yetty_yfont PUBLIC ${FREETYPE_ALL_LIBS} webgpu yetty_ycdb)

# Embed shaders via incbin (or stubs on Emscripten)
incbin_add_resources(yetty_yfont
    raster_font_shader "${CMAKE_CURRENT_SOURCE_DIR}/raster-font.wgsl"
    ms_msdf_font_shader "${CMAKE_CURRENT_SOURCE_DIR}/ms-msdf-font.wgsl"
    msdf_font_shader "${CMAKE_CURRENT_SOURCE_DIR}/msdf-font.wgsl"
)
