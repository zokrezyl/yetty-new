add_subdirectory(sdf)
add_subdirectory(core)
add_subdirectory(yaml)
add_subdirectory(yplot)

add_library(yetty_ypaint INTERFACE)

target_link_libraries(yetty_ypaint INTERFACE
    ypaint_sdf
    ypaint_core
    ypaint_yaml
    ypaint_yplot
)
