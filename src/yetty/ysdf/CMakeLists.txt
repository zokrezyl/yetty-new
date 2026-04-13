add_library(ypaint_sdf STATIC
    funcs.gen.c
    aabb.gen.c
    yaml.gen.c
)

target_include_directories(ypaint_sdf PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
)

target_link_libraries(ypaint_sdf PUBLIC yaml)
