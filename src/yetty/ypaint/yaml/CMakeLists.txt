add_library(ypaint_yaml STATIC
    ypaint-yaml.gen.c
)

target_include_directories(ypaint_yaml PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
)

target_link_libraries(ypaint_yaml PUBLIC ypaint_core ypaint_sdf yaml)
