add_library(yetty_yms_msdf STATIC
    yms-msdf.c
)

target_include_directories(yetty_yms_msdf PUBLIC
    ${YETTY_ROOT}/include
)

target_link_libraries(yetty_yms_msdf PUBLIC
    yetty_ycdb
    yetty_ymsdf_gen_cpu
)
