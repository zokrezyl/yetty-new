# VNC-like remote display client/server

add_library(yetty_vnc STATIC
    vnc-client.c
    vnc-server.c
    vnc-viewer.c
)

target_include_directories(yetty_vnc PUBLIC
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(yetty_vnc PUBLIC
    webgpu
    turbojpeg-static
    yetty_ycore
    yetty_yrender_utils
)

if(YETTY_ENABLE_LIB_OPENH264)
    target_compile_definitions(yetty_vnc PUBLIC YETTY_HAS_YVIDEO=1)
    target_link_libraries(yetty_vnc PUBLIC yetty_yvideo)
endif()
