include(${YETTY_ROOT}/build-tools/cmake/libs/msdfgen.cmake)
include(${YETTY_ROOT}/build-tools/cmake/FreeType.cmake)

add_library(yetty_ymsdf_gen_cpu STATIC
    ymsdf-gen-cpu.cpp
)

target_include_directories(yetty_ymsdf_gen_cpu PUBLIC
    ${YETTY_ROOT}/include
    ${FREETYPE_INCLUDE_DIR}
)

target_link_libraries(yetty_ymsdf_gen_cpu PUBLIC
    msdfgen::msdfgen-core
    msdfgen::msdfgen-ext
    ${FREETYPE_ALL_LIBS}
)
