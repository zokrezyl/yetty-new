# libmagic (file(1) library) — file-type detection from magic numbers.
#
# Built as an ExternalProject from the official file(1) distribution tarball.
# Produces: imported STATIC target `magic` + cache var LIBMAGIC_MGC_PATH
# pointing at the compiled magic database (share/misc/magic.mgc).

if(TARGET magic)
    return()
endif()

include(ExternalProject)
include(ProcessorCount)

ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 4)
endif()

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(LIBMAGIC_CC "ccache ${CMAKE_C_COMPILER}")
else()
    set(LIBMAGIC_CC "${CMAKE_C_COMPILER}")
endif()

set(LIBMAGIC_VERSION "5.45")

if(DEFINED FETCHCONTENT_SOURCE_DIR_LIBMAGIC)
    set(LIBMAGIC_SOURCE_ARGS
        SOURCE_DIR "${FETCHCONTENT_SOURCE_DIR_LIBMAGIC}"
        DOWNLOAD_COMMAND ""
    )
    message(STATUS "libmagic: using pre-fetched source at ${FETCHCONTENT_SOURCE_DIR_LIBMAGIC}")
else()
    set(LIBMAGIC_SOURCE_ARGS
        URL https://astron.com/pub/file/file-${LIBMAGIC_VERSION}.tar.gz
    )
endif()

# Use official distribution tarball: pre-generated configure, no autotools
# required on the build host. --disable-maintainer-mode stops make from trying
# to regenerate aclocal.m4 from slightly newer source timestamps.
ExternalProject_Add(libmagic_ext
    ${LIBMAGIC_SOURCE_ARGS}
    PREFIX          ${CMAKE_BINARY_DIR}/_deps/libmagic
    UPDATE_DISCONNECTED TRUE

    PATCH_COMMAND
        find <SOURCE_DIR> -name "aclocal.m4" -o -name "configure"
            -o -name "Makefile.in" -o -name "config.h.in" | xargs touch

    CONFIGURE_COMMAND
        ${CMAKE_COMMAND} -E env "CC=${LIBMAGIC_CC}"
        <SOURCE_DIR>/configure
            --prefix=<INSTALL_DIR>
            --disable-shared
            --enable-static
            --disable-libseccomp
            --disable-bzlib
            --disable-xzlib
            --disable-zstdlib
            --disable-lzlib
            --disable-maintainer-mode
            "CFLAGS=-fPIC -O3 -DNDEBUG"

    BUILD_COMMAND
        ${CMAKE_COMMAND} -E env
            "CPPFLAGS=-I${zlib_SOURCE_DIR} -I${zlib_BINARY_DIR}"
            "LDFLAGS=-L${zlib_BINARY_DIR}"
        make -j${NPROC} LIBS=-lz
    INSTALL_COMMAND   make install
    BUILD_IN_SOURCE   TRUE

    BUILD_BYPRODUCTS
        <INSTALL_DIR>/lib/libmagic.a
        <INSTALL_DIR>/include/magic.h
        <INSTALL_DIR>/share/misc/magic.mgc
)

add_dependencies(libmagic_ext zlibstatic)

ExternalProject_Get_Property(libmagic_ext INSTALL_DIR)

file(MAKE_DIRECTORY ${INSTALL_DIR}/include)

add_library(magic STATIC IMPORTED GLOBAL)
set_target_properties(magic PROPERTIES
    IMPORTED_LOCATION ${INSTALL_DIR}/lib/libmagic.a
    INTERFACE_INCLUDE_DIRECTORIES ${INSTALL_DIR}/include
    INTERFACE_LINK_LIBRARIES zlibstatic
)
add_dependencies(magic libmagic_ext)

set(LIBMAGIC_MGC_PATH "${INSTALL_DIR}/share/misc/magic.mgc" CACHE FILEPATH
    "Path to compiled magic database" FORCE)
