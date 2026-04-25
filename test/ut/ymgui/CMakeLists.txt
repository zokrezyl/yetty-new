# ymgui unit tests
#
# Standalone C programs run via ctest. Enable with
# -DYETTY_ENABLE_FEATURE_TESTS=ON.

enable_testing()

add_executable(ut-ymgui-b64
    test-b64.c
    ${YETTY_ROOT}/src/yetty/ymgui/frontend/ymgui_encode.c
)

target_include_directories(ut-ymgui-b64 PRIVATE
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
    ${YETTY_ROOT}/src/yetty/ymgui/frontend
)

target_link_libraries(ut-ymgui-b64 PRIVATE yetty_ycore)
target_compile_features(ut-ymgui-b64 PRIVATE c_std_11)

add_test(NAME ymgui_b64 COMMAND ut-ymgui-b64)
