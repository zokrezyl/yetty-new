# libpng — PNG image library.
#
# Consumes a prebuilt static lib + headers from the 3rdparty release
# tarball published by build-3rdparty-libpng.yml. The from-source build
# (cmake; depends on prebuilt zlib at compile time) lives in
# build-tools/3rdparty/libpng/_build.sh.
#
# Exposed target: png_static — IMPORTED static archive (same name the
# from-source CPM build exported).

include_guard(GLOBAL)
include(${YETTY_ROOT}/build-tools/cmake/3rdparty-fetch.cmake)

if(TARGET png_static)
    return()
endif()

if(WIN32)
    message(FATAL_ERROR
        "libpng: no windows-x86_64 tarball is published yet — yetty.exe is \
being switched to native MSVC and the libpng MSVC build path will land \
together with that work (see the windows-libs-msvc branch).")
endif()

# zlib must resolve before us — png_static carries unresolved zlib refs.
include(${CMAKE_CURRENT_LIST_DIR}/zlib.cmake)

yetty_3rdparty_fetch(libpng _LIBPNG_DIR)

if(NOT EXISTS "${_LIBPNG_DIR}/lib/libpng.a")
    message(FATAL_ERROR "libpng: libpng.a not found in ${_LIBPNG_DIR}/lib/ — tarball layout changed?")
endif()

add_library(png_static STATIC IMPORTED GLOBAL)
set_target_properties(png_static PROPERTIES
    IMPORTED_LOCATION "${_LIBPNG_DIR}/lib/libpng.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_LIBPNG_DIR}/include"
    INTERFACE_LINK_LIBRARIES "ZLIB::ZLIB"
)

set(PNG_FOUND          TRUE                          CACHE BOOL    "" FORCE)
set(PNG_INCLUDE_DIRS   "${_LIBPNG_DIR}/include"      CACHE PATH    "" FORCE)
set(PNG_PNG_INCLUDE_DIR "${_LIBPNG_DIR}/include"     CACHE PATH    "" FORCE)
set(PNG_LIBRARY        png_static                    CACHE STRING  "" FORCE)
set(PNG_LIBRARIES      png_static                    CACHE STRING  "" FORCE)

message(STATUS "libpng: prebuilt v${YETTY_3RDPARTY_libpng_VERSION} (${_LIBPNG_DIR}/lib/libpng.a)")
