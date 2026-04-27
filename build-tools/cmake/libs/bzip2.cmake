# bzip2 — bzip2 compression.
#
# Consumes a prebuilt static lib + header from the 3rdparty release
# tarball published by build-3rdparty-bzip2.yml. The from-source build
# (compiles the 7 .c files directly) lives in
# build-tools/3rdparty/bzip2/_build.sh.
#
# Exposed target: bz2_static — IMPORTED static archive (same name the
# from-source bundle in FreeType.cmake exposed).

include_guard(GLOBAL)
include(${YETTY_ROOT}/build-tools/cmake/3rdparty-fetch.cmake)

if(TARGET bz2_static)
    return()
endif()

if(WIN32)
    message(FATAL_ERROR
        "bzip2: no windows-x86_64 tarball is published yet — yetty.exe is \
being switched to native MSVC and the bzip2 MSVC build path will land \
together with that work (see the windows-libs-msvc branch).")
endif()

yetty_3rdparty_fetch(bzip2 _BZIP2_DIR)

if(NOT EXISTS "${_BZIP2_DIR}/lib/libbz2.a")
    message(FATAL_ERROR "bzip2: libbz2.a not found in ${_BZIP2_DIR}/lib/ — tarball layout changed?")
endif()

add_library(bz2_static STATIC IMPORTED GLOBAL)
set_target_properties(bz2_static PROPERTIES
    IMPORTED_LOCATION "${_BZIP2_DIR}/lib/libbz2.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_BZIP2_DIR}/include"
)
set(BZIP2_LIBRARIES   bz2_static                CACHE INTERNAL "")
set(BZIP2_INCLUDE_DIR "${_BZIP2_DIR}/include"   CACHE INTERNAL "")

message(STATUS "bzip2: prebuilt v${YETTY_3RDPARTY_bzip2_VERSION} (${_BZIP2_DIR}/lib/libbz2.a)")
