# zlib — compression library.
#
# Consumes a prebuilt static lib + headers from the 3rdparty release
# tarball published by build-3rdparty-zlib.yml. The from-source build
# (cmake) lives in build-tools/3rdparty/zlib/_build.sh — uses upstream
# madler/zlib v1.3.1 on every target.
#
# Exposed targets:
#   ZLIB::ZLIB     — INTERFACE alias find_package(ZLIB) consumers expect
#   zlibstatic     — bare imported static, kept for old-build compat
#   zlib           — alias of zlibstatic, kept for zlib-ng-style consumers

include_guard(GLOBAL)
include(${YETTY_ROOT}/build-tools/cmake/3rdparty-fetch.cmake)

if(TARGET zlib OR TARGET zlibstatic OR TARGET ZLIB::ZLIB)
    return()
endif()

if(WIN32)
    message(FATAL_ERROR
        "zlib: no windows-x86_64 tarball is published yet — yetty.exe is \
being switched to native MSVC and the zlib MSVC build path will land \
together with that work (see the windows-libs-msvc branch).")
endif()

yetty_3rdparty_fetch(zlib _ZLIB_DIR)

if(NOT EXISTS "${_ZLIB_DIR}/lib/libz.a")
    message(FATAL_ERROR
        "zlib: libz.a not found in ${_ZLIB_DIR}/lib/ — tarball layout changed?")
endif()
if(NOT EXISTS "${_ZLIB_DIR}/include/zlib.h")
    message(FATAL_ERROR "zlib: zlib.h not found in ${_ZLIB_DIR}/include/")
endif()

add_library(zlibstatic STATIC IMPORTED GLOBAL)
set_target_properties(zlibstatic PROPERTIES
    IMPORTED_LOCATION "${_ZLIB_DIR}/lib/libz.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_ZLIB_DIR}/include"
)
add_library(zlib ALIAS zlibstatic)
add_library(ZLIB::ZLIB ALIAS zlibstatic)

# find_package(ZLIB) compat — some downstream cmake reads these.
set(ZLIB_FOUND        TRUE                        CACHE BOOL    "" FORCE)
set(ZLIB_INCLUDE_DIR  "${_ZLIB_DIR}/include"      CACHE PATH    "" FORCE)
set(ZLIB_INCLUDE_DIRS "${_ZLIB_DIR}/include"      CACHE PATH    "" FORCE)
set(ZLIB_LIBRARY      "${_ZLIB_DIR}/lib/libz.a"   CACHE FILEPATH "" FORCE)
set(ZLIB_LIBRARIES    "${_ZLIB_DIR}/lib/libz.a"   CACHE STRING  "" FORCE)
set(ZLIB_ROOT         "${_ZLIB_DIR}"              CACHE PATH    "" FORCE)

message(STATUS "zlib: prebuilt v${YETTY_3RDPARTY_zlib_VERSION} (${_ZLIB_DIR}/lib/libz.a)")
