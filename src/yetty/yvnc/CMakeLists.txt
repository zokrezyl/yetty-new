# VNC-like remote display client/server

add_library(yetty_vnc STATIC
    vnc-client.c
    vnc-server.c
)

target_include_directories(yetty_vnc PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(yetty_vnc PUBLIC
    webgpu
    turbojpeg-static
)
