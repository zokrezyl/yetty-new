# ypdf - PDF → ypaint buffer rendering
add_library(yetty_ypdf STATIC
    pdf-content-parser.c
    pdf-renderer.c
)

target_include_directories(yetty_ypdf PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
)

target_link_libraries(yetty_ypdf PUBLIC
    yetty_ycore
    yetty_ypaint_core
    yetty_yfont
    yetty_ysdf
    pdfio_lib
)
