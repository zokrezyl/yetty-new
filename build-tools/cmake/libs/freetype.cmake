# freetype — font rasterization.
#
# Consumes a prebuilt static lib + headers from the 3rdparty release
# tarball published by build-3rdparty-freetype.yml. The from-source
# build (cmake; brotli+bzip2+png deps disabled, zlib via prebuilt zlib
# tarball at compile time) lives in build-tools/3rdparty/freetype/_build.sh.
#
# Exposed targets:
#   freetype           — IMPORTED static archive
#   Freetype::Freetype — alias find_package(Freetype) consumers expect

include_guard(GLOBAL)
include(${YETTY_ROOT}/build-tools/cmake/3rdparty-fetch.cmake)

if(TARGET freetype OR TARGET Freetype::Freetype)
    return()
endif()

if(WIN32)
    message(FATAL_ERROR
        "freetype: no windows-x86_64 tarball is published yet — yetty.exe is \
being switched to native MSVC and the freetype MSVC build path will land \
together with that work (see the windows-libs-msvc branch).")
endif()

# zlib resolves first — freetype.a has unresolved zlib symbols.
include(${CMAKE_CURRENT_LIST_DIR}/zlib.cmake)

yetty_3rdparty_fetch(freetype _FREETYPE_DIR)

if(NOT EXISTS "${_FREETYPE_DIR}/lib/libfreetype.a")
    message(FATAL_ERROR "freetype: libfreetype.a not found in ${_FREETYPE_DIR}/lib/ — tarball layout changed?")
endif()

# Upstream installs to include/freetype2/, but we ship include/ flat.
# Accept either layout.
set(_FREETYPE_INC "${_FREETYPE_DIR}/include")
if(EXISTS "${_FREETYPE_DIR}/include/freetype2/ft2build.h")
    set(_FREETYPE_INC "${_FREETYPE_DIR}/include;${_FREETYPE_DIR}/include/freetype2")
endif()

add_library(freetype STATIC IMPORTED GLOBAL)
set_target_properties(freetype PROPERTIES
    IMPORTED_LOCATION "${_FREETYPE_DIR}/lib/libfreetype.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_FREETYPE_INC}"
    INTERFACE_LINK_LIBRARIES "ZLIB::ZLIB"
)
add_library(Freetype::Freetype ALIAS freetype)

set(FREETYPE_INCLUDE_DIR "${_FREETYPE_INC}"                    CACHE INTERNAL "")
set(FREETYPE_LIBRARY     "${_FREETYPE_DIR}/lib/libfreetype.a"  CACHE INTERNAL "")
set(FREETYPE_FOUND       TRUE                                  CACHE BOOL    "" FORCE)

# Bundle var that downstream code (yetty cmake targets) reads — a
# sane link order for a final executable that uses freetype.
# yetty's previous bundle pulled in brotli + bzip2 + libpng + zlib;
# we keep the same shape so consumers don't change.
set(FREETYPE_ALL_LIBS
    freetype
    png_static
    brotlidec
    brotlicommon
    bz2_static
    ZLIB::ZLIB
    $<$<NOT:$<BOOL:${WIN32}>>:m>
    CACHE INTERNAL "All FreeType static libs in link order"
)

message(STATUS "freetype: prebuilt v${YETTY_3RDPARTY_freetype_VERSION} (${_FREETYPE_DIR}/lib/libfreetype.a)")
