add_library(yetty_term
    terminal.c
    text-layer.c
)

target_include_directories(yetty_term PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(yetty_term
    vterm
)
