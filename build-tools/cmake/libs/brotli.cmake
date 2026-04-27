# brotli — Google's brotli compression.
#
# Consumes prebuilt static libs + headers from the 3rdparty release
# tarball published by build-3rdparty-brotli.yml. The from-source build
# (cmake) lives in build-tools/3rdparty/brotli/_build.sh.
#
# Exposed targets: brotlicommon, brotlidec, brotlienc — IMPORTED static
# archives, same names the upstream cmake exports.

include_guard(GLOBAL)
include(${YETTY_ROOT}/build-tools/cmake/3rdparty-fetch.cmake)

if(TARGET brotlidec)
    return()
endif()

if(WIN32)
    message(FATAL_ERROR
        "brotli: no windows-x86_64 tarball is published yet — yetty.exe is \
being switched to native MSVC and the brotli MSVC build path will land \
together with that work (see the windows-libs-msvc branch).")
endif()

yetty_3rdparty_fetch(brotli _BROTLI_DIR)

foreach(_LIB libbrotlicommon.a libbrotlidec.a libbrotlienc.a)
    if(NOT EXISTS "${_BROTLI_DIR}/lib/${_LIB}")
        message(FATAL_ERROR "brotli: ${_LIB} not found in ${_BROTLI_DIR}/lib/ — tarball layout changed?")
    endif()
endforeach()

add_library(brotlicommon STATIC IMPORTED GLOBAL)
set_target_properties(brotlicommon PROPERTIES
    IMPORTED_LOCATION "${_BROTLI_DIR}/lib/libbrotlicommon.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_BROTLI_DIR}/include"
)
add_library(brotlidec STATIC IMPORTED GLOBAL)
set_target_properties(brotlidec PROPERTIES
    IMPORTED_LOCATION "${_BROTLI_DIR}/lib/libbrotlidec.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_BROTLI_DIR}/include"
    INTERFACE_LINK_LIBRARIES "brotlicommon"
)
add_library(brotlienc STATIC IMPORTED GLOBAL)
set_target_properties(brotlienc PROPERTIES
    IMPORTED_LOCATION "${_BROTLI_DIR}/lib/libbrotlienc.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_BROTLI_DIR}/include"
    INTERFACE_LINK_LIBRARIES "brotlicommon"
)

# Compat for FreeType.cmake's old bundle-mode names.
set(BROTLIDEC_LIBRARIES "brotlidec;brotlicommon" CACHE INTERNAL "")
set(BROTLI_INCLUDE_DIR  "${_BROTLI_DIR}/include" CACHE INTERNAL "")

message(STATUS "brotli: prebuilt v${YETTY_3RDPARTY_brotli_VERSION} (${_BROTLI_DIR}/lib/)")
