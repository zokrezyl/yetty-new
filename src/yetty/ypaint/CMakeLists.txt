# ypaint layer - rich content rendering
# Note: core moved to ypaint-core, SDF/yaml moved to ysdf

add_library(yetty_ypaint STATIC
    flyweight.c
    ypaint-canvas.c
)

target_link_libraries(yetty_ypaint PUBLIC
    yetty_ypaint_core
    yetty_ysdf
    yetty_ymsdf_gen
)
# Note: yetty_ypaint references yplatform_mkdir_p / yplatform_file_exists
# (per-platform shared sources). Final executables already include
# YETTY_YPLATFORM_THREAD_SOURCES, so these symbols resolve at link time.
