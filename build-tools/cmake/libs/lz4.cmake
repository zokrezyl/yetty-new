# lz4 — fast compression.
#
# Consumes a prebuilt static lib + headers from the 3rdparty release
# tarball published by build-3rdparty-lz4.yml. The from-source build
# (cmake at build/cmake/) lives in build-tools/3rdparty/lz4/_build.sh.
#
# Exposed target: lz4_static — IMPORTED static archive. Same name the
# from-source CPM build exported.

include_guard(GLOBAL)
include(${YETTY_ROOT}/build-tools/cmake/3rdparty-fetch.cmake)

if(TARGET lz4_static)
    return()
endif()

if(WIN32)
    message(FATAL_ERROR
        "lz4: no windows-x86_64 tarball is published yet — yetty.exe is \
being switched to native MSVC and the lz4 MSVC build path will land \
together with that work (see the windows-libs-msvc branch).")
endif()

yetty_3rdparty_fetch(lz4 _LZ4_DIR)

if(NOT EXISTS "${_LZ4_DIR}/lib/liblz4.a")
    message(FATAL_ERROR "lz4: liblz4.a not found in ${_LZ4_DIR}/lib/ — tarball layout changed?")
endif()

add_library(lz4_static STATIC IMPORTED GLOBAL)
set_target_properties(lz4_static PROPERTIES
    IMPORTED_LOCATION "${_LZ4_DIR}/lib/liblz4.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_LZ4_DIR}/include"
)

message(STATUS "lz4: prebuilt v${YETTY_3RDPARTY_lz4_VERSION} (${_LZ4_DIR}/lib/liblz4.a)")
