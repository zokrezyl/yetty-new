# ymgui — Dear ImGui ↔ yetty bridge.
#
# Two pieces:
#   frontend/  — client-side library the app links against. Ships an
#                ImGui renderer backend (imgui_impl_yetty.*) and the
#                OSC/encode helpers. Needs Dear ImGui sources.
#   (no backend/ subdir — the yetty-side receiver is the ymgui-layer
#    terminal layer in src/yetty/yterm/. A layer, not a side-car lib.)
#
# Guarded by YETTY_ENABLE_FEATURE_YMGUI. The frontend also needs Dear
# ImGui sources to be available — set YMGUI_IMGUI_DIR to point at a
# vendored copy (default: <repo>/tmp/imgui-1.92.7).

if(NOT DEFINED YMGUI_IMGUI_DIR)
    set(YMGUI_IMGUI_DIR "${YETTY_ROOT}/tmp/imgui-1.92.7"
        CACHE PATH "Path to Dear ImGui sources for the ymgui frontend")
endif()

if(EXISTS "${YMGUI_IMGUI_DIR}/imgui.h")
    add_subdirectory(frontend)
else()
    message(STATUS
        "ymgui: Dear ImGui not found at ${YMGUI_IMGUI_DIR} — frontend skipped "
        "(set -DYMGUI_IMGUI_DIR=/path/to/imgui to build).")
endif()
