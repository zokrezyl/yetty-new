# ygui C demos — each builds a tiny program that uses the ygui widget API.
# ygui accumulates primitives into a ypaint-core buffer and ships them as
# base64-encoded OSC sequences to a running yetty instance, which decodes
# them via yetty_ypaint_core_buffer_create_from_base64 and renders.
# See src/yetty/ygui/ydraw-capi.gen.{h,c} for the ypaint buffer shim.

# Shared support object: ytrace + ymutex/term platform glue. Compiled once
# and linked into each demo exec so we avoid static-archive ordering issues
# (ygui/ypaint-core reference these symbols; they need to be in the final
# exec object list, not hidden deeper in the archive chain).
add_library(demo_ygui_support OBJECT
    ${YETTY_ROOT}/src/yetty/ytrace.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/thread.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/term.c
)
target_include_directories(demo_ygui_support PUBLIC ${YETTY_ROOT}/include)

add_subdirectory(01_button_test)
add_subdirectory(02_coord_debug)
add_subdirectory(03_all_widgets)
add_subdirectory(04_edge_test)
