# libcurl (system) — used by ycat for http(s):// fetching.
#
# System libcurl is available on Linux/macOS out of the box; Windows would
# need a build-from-source fallback (not addressed here yet). Exports target
# `CURL::libcurl`.

message(STATUS "libcurl: configuring")

if(TARGET CURL::libcurl)
    message(STATUS "libcurl: CURL::libcurl already exists")
    return()
endif()

find_package(CURL)
if(NOT CURL_FOUND)
    message(FATAL_ERROR
        "libcurl not found. Install libcurl4-openssl-dev (Debian/Ubuntu), "
        "libcurl-devel (Fedora), or set YETTY_ENABLE_LIB_LIBCURL=OFF.")
endif()
message(STATUS "libcurl: found v${CURL_VERSION_STRING} at ${CURL_LIBRARIES}")

# Promote to a global imported target so downstream subdirectories can link
# against it. Some CMake versions create CURL::libcurl at local scope only.
if(TARGET CURL::libcurl)
    message(STATUS "libcurl: promoting CURL::libcurl to GLOBAL")
    set_target_properties(CURL::libcurl PROPERTIES IMPORTED_GLOBAL TRUE)
else()
    message(STATUS "libcurl: creating CURL::libcurl INTERFACE wrapper")
    add_library(CURL::libcurl INTERFACE IMPORTED GLOBAL)
    set_target_properties(CURL::libcurl PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CURL_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES      "${CURL_LIBRARIES}")
endif()
