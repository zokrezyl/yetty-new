# ypaint layer - rich content rendering
# Note: core moved to ypaint-core, SDF/yaml moved to ysdf

add_library(yetty_ypaint STATIC
    flyweight.c
    ypaint-canvas.c
)

target_link_libraries(yetty_ypaint PUBLIC
    yetty_ypaint_core
    yetty_ysdf
)
