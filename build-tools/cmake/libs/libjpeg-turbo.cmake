# libjpeg-turbo — JPEG codec with SIMD optimisations.
#
# Consumes a prebuilt static lib + headers from the 3rdparty release
# tarball published by build-3rdparty-libjpeg-turbo.yml. The from-source
# build (cmake + NASM) lives in build-tools/3rdparty/libjpeg-turbo/_build.sh.
#
# Exposed target: turbojpeg-static — IMPORTED static archive (same name
# the from-source ExternalProject build exported).

include_guard(GLOBAL)
include(${YETTY_ROOT}/build-tools/cmake/3rdparty-fetch.cmake)

if(TARGET turbojpeg-static)
    return()
endif()

if(WIN32)
    message(FATAL_ERROR
        "libjpeg-turbo: no windows-x86_64 tarball is published yet — yetty.exe \
is being switched to native MSVC and the libjpeg-turbo MSVC build path will \
land together with that work (see the windows-libs-msvc branch).")
endif()

yetty_3rdparty_fetch(libjpeg-turbo _LIBJPEG_DIR)

if(NOT EXISTS "${_LIBJPEG_DIR}/lib/libturbojpeg.a")
    message(FATAL_ERROR "libjpeg-turbo: libturbojpeg.a not found in ${_LIBJPEG_DIR}/lib/ — tarball layout changed?")
endif()

add_library(turbojpeg-static STATIC IMPORTED GLOBAL)
set_target_properties(turbojpeg-static PROPERTIES
    IMPORTED_LOCATION "${_LIBJPEG_DIR}/lib/libturbojpeg.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_LIBJPEG_DIR}/include"
)

if(EXISTS "${_LIBJPEG_DIR}/lib/libjpeg.a")
    add_library(jpeg-static STATIC IMPORTED GLOBAL)
    set_target_properties(jpeg-static PROPERTIES
        IMPORTED_LOCATION "${_LIBJPEG_DIR}/lib/libjpeg.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_LIBJPEG_DIR}/include"
    )
endif()

set(JPEG_FOUND        TRUE                       CACHE BOOL   "" FORCE)
set(JPEG_INCLUDE_DIRS "${_LIBJPEG_DIR}/include"  CACHE PATH   "" FORCE)
set(JPEG_LIBRARIES    turbojpeg-static           CACHE STRING "" FORCE)

message(STATUS "libjpeg-turbo: prebuilt v${YETTY_3RDPARTY_libjpeg-turbo_VERSION} (${_LIBJPEG_DIR}/lib/libturbojpeg.a)")
