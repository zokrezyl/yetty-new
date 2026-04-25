add_executable(demo-ymgui-01-demo-window main.cpp)
target_link_libraries(demo-ymgui-01-demo-window PRIVATE ymgui_frontend)
target_compile_features(demo-ymgui-01-demo-window PRIVATE cxx_std_17)
