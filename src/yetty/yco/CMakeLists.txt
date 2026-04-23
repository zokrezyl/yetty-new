add_library(yetty_yco STATIC
    co.c
)

target_include_directories(yetty_yco PUBLIC
    ${YETTY_ROOT}/include
)

target_link_libraries(yetty_yco PUBLIC
    co
)
