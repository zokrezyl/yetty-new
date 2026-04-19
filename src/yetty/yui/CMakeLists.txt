add_library(yetty_yui STATIC
    tile.c
    view.c
    workspace.c
)

# stdatomic.h requires C11
set_target_properties(yetty_yui PROPERTIES C_STANDARD 11)

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
