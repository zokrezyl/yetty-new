add_library(yetty_yexpr STATIC
    yexpr.c
)

target_include_directories(yetty_yexpr PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
