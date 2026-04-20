add_library(yetty_ypaint_yaml STATIC
    ypaint-yaml.c
)

target_include_directories(yetty_ypaint_yaml PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
)

target_link_libraries(yetty_ypaint_yaml PUBLIC
    yaml
    yetty_ypaint_core
    yetty_ysdf
    yetty_yplot
    yetty_yfsvm
)
