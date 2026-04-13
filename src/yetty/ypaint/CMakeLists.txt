# ypaint layer - rich content rendering
# Note: core moved to ypaint-core, SDF/yaml moved to ysdf

add_library(yetty_ypaint STATIC
    ypaint-canvas.c
)

target_link_libraries(yetty_ypaint PUBLIC
    ypaint_core
    ypaint_sdf
)
