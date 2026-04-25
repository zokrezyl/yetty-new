# ymgui C++ demos — each program pushes ImGui frames as OSC to a running
# yetty instance. They depend on ymgui_frontend, which itself is only
# built when Dear ImGui sources are present at YMGUI_IMGUI_DIR (see
# src/yetty/ymgui/CMakeLists.txt). If the frontend wasn't built, skip
# the demos silently — otherwise the build fails on missing imgui.h in
# worktrees that don't carry the vendored sources.
if(TARGET ymgui_frontend)
    add_subdirectory(01_demo_window)
else()
    message(STATUS
        "ymgui demos skipped — ymgui_frontend target not built "
        "(Dear ImGui sources not found at ${YMGUI_IMGUI_DIR}).")
endif()
