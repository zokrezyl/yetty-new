add_executable(demo-ygui-08-slider main.c $<TARGET_OBJECTS:demo_ygui_support>)
target_link_libraries(demo-ygui-08-slider PRIVATE ygui)
target_compile_features(demo-ygui-08-slider PRIVATE c_std_11)
