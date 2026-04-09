# yetty font subsystem (C)
add_library(yetty_font STATIC
    raster-font.c
)

target_include_directories(yetty_font PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
    ${FREETYPE_INCLUDE_DIR}
)

target_link_libraries(yetty_font PUBLIC ${FREETYPE_ALL_LIBS} ytrace::ytrace)
