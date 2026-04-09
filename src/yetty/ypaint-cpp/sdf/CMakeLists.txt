add_library(ypaint_sdf STATIC
    ypaint-sdf-prim.gen.c
    ypaint-sdf-aabb.gen.c
)

target_include_directories(ypaint_sdf PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
)

target_link_libraries(ypaint_sdf PUBLIC ypaint_core)
