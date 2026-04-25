add_executable(ypaint-bench main.c)
target_link_libraries(ypaint-bench PRIVATE yetty_yface yetty_ycore)
target_compile_options(ypaint-bench PRIVATE -Wall -Wextra)
