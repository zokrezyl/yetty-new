# yvideo — H.264 encoder/decoder facade over openh264.
#
# Port of yetty-poc/src/yetty/yvideo. The implementations are C++ because
# openh264 exposes virtual C++ classes, but the public API in
# include/yetty/yvideo/*.h is pure C — everything else in yetty-perf is C and
# calls through that facade.

add_library(yetty_yvideo STATIC
    encoder.cpp
    decoder.cpp
)

target_include_directories(yetty_yvideo PUBLIC
    ${YETTY_ROOT}/include
)

target_link_libraries(yetty_yvideo PUBLIC
    openh264
)
