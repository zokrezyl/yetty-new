# yetty_yface — streaming OSC stream:
#   bytes → LZ4F → base64 → out_buf
#   chars → base64 → LZ4F → in_buf
#
# Depends on: yetty_ycore (growable buffer), lz4_static (LZ4F compressor).

add_library(yetty_yface STATIC yface.c)

target_include_directories(yetty_yface PUBLIC
    ${YETTY_ROOT}/include
)

target_link_libraries(yetty_yface PUBLIC
    yetty_ycore
    lz4_static
)

set_target_properties(yetty_yface PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

install(FILES ${YETTY_ROOT}/include/yetty/yface/yface.h
        DESTINATION include/yetty/yface)
install(TARGETS yetty_yface ARCHIVE DESTINATION lib)
