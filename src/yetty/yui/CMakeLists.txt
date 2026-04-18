add_library(yetty_yui STATIC
    tile.c
    view.c
    workspace.c
)

target_include_directories(yetty_yui
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(yetty_yui
    PRIVATE
        webgpu
        yetty_term
)
