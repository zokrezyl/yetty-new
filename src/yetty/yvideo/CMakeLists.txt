# yvideo — H.264 encoder/decoder wrapping openh264.
#
# Port of yetty-poc/src/yetty/yvideo. openh264 ships both a C++ virtual-class
# interface and a C COM-style vtable interface (`ISVCEncoder = const
# ISVCEncoderVtbl*`) — we use the C one so these TUs stay pure C and match
# the rest of yetty-perf.

add_library(yetty_yvideo STATIC
    encoder.c
    decoder.c
)

target_include_directories(yetty_yvideo PUBLIC
    ${YETTY_ROOT}/include
)

target_link_libraries(yetty_yvideo PUBLIC
    openh264
)
