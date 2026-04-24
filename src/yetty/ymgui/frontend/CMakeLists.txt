# ymgui_frontend — links into the client app, not into yetty itself.
#
# Three translation units:
#   ymgui_encode.c         — pure C. base64 + growable buf + OSC write.
#   imgui_impl_yetty.cpp   — thin C++ shim on top of Dear ImGui.
#   (plus vendored Dear ImGui sources, to keep this build self-contained.)

set(YMGUI_IMGUI_SOURCES
    ${YMGUI_IMGUI_DIR}/imgui.cpp
    ${YMGUI_IMGUI_DIR}/imgui_draw.cpp
    ${YMGUI_IMGUI_DIR}/imgui_tables.cpp
    ${YMGUI_IMGUI_DIR}/imgui_widgets.cpp
    ${YMGUI_IMGUI_DIR}/imgui_demo.cpp
)

add_library(ymgui_frontend STATIC
    ymgui_encode.c
    imgui_impl_yetty.cpp
    ${YMGUI_IMGUI_SOURCES}
)

target_include_directories(ymgui_frontend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${YETTY_ROOT}/include
    ${YMGUI_IMGUI_DIR}
)

set_target_properties(ymgui_frontend PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Public headers consumers need:
#   <yetty/ymgui/wire.h>              (installed with core headers)
#   "imgui_impl_yetty.h"              (this dir)
#   "ymgui_encode.h"                  (this dir — optional, for custom transports)

install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui_impl_yetty.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ymgui_encode.h
    DESTINATION include/yetty/ymgui)

install(TARGETS ymgui_frontend ARCHIVE DESTINATION lib)
