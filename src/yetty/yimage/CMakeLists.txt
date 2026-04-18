add_library(yetty_yimage STATIC
    yimage.c
)

target_include_directories(yetty_yimage PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
