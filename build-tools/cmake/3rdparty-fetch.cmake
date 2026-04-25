# 3rdparty-fetch.cmake
#
# Downloads the prebuilt 3rdparty-library tarballs published by
# build-3rdparty.yml (GitHub release tag `3rdparty-${YETTY_3RDPARTY_VERSION}`)
# and extracts them into the build tree so the rest of the build can consume
# them as imported static targets.
#
# Output layout (one subdir per library):
#   ${CMAKE_BINARY_DIR}/3rdparty/<lib>/lib/...
#   ${CMAKE_BINARY_DIR}/3rdparty/<lib>/include/...
#
# Pinning: version is read from ${YETTY_ROOT}/3rdparty.version (bumped when
# you cut a new 3rdparty-X.Y.Z release). Override via cmake/env var
# YETTY_3RDPARTY_VERSION.
#
# URL override: YETTY_3RDPARTY_URL_BASE (default: GitHub releases for the
# upstream repo).

include_guard(GLOBAL)

#-----------------------------------------------------------------------------
# Resolve version + url base
#-----------------------------------------------------------------------------
if(NOT DEFINED YETTY_3RDPARTY_VERSION)
    if(DEFINED ENV{YETTY_3RDPARTY_VERSION})
        set(YETTY_3RDPARTY_VERSION "$ENV{YETTY_3RDPARTY_VERSION}")
    else()
        set(_VER_FILE "${YETTY_ROOT}/3rdparty.version")
        if(NOT EXISTS "${_VER_FILE}")
            message(FATAL_ERROR "3rdparty-fetch: ${_VER_FILE} not found")
        endif()
        file(READ "${_VER_FILE}" YETTY_3RDPARTY_VERSION)
        string(STRIP "${YETTY_3RDPARTY_VERSION}" YETTY_3RDPARTY_VERSION)
    endif()
endif()
set(YETTY_3RDPARTY_VERSION "${YETTY_3RDPARTY_VERSION}"
    CACHE STRING "Pinned 3rdparty release version")

if(NOT DEFINED YETTY_3RDPARTY_URL_BASE)
    if(DEFINED ENV{YETTY_3RDPARTY_URL_BASE})
        set(YETTY_3RDPARTY_URL_BASE "$ENV{YETTY_3RDPARTY_URL_BASE}")
    else()
        set(YETTY_3RDPARTY_URL_BASE "https://github.com/zokrezyl/yetty/releases/download")
    endif()
endif()
set(YETTY_3RDPARTY_URL_BASE "${YETTY_3RDPARTY_URL_BASE}"
    CACHE STRING "Base URL for 3rdparty downloads")

if(NOT DEFINED YETTY_3RDPARTY_CACHE_DIR)
    if(DEFINED ENV{YETTY_3RDPARTY_CACHE_DIR})
        set(YETTY_3RDPARTY_CACHE_DIR "$ENV{YETTY_3RDPARTY_CACHE_DIR}")
    else()
        # Cache across builds (not wiped when build dir is cleaned)
        set(YETTY_3RDPARTY_CACHE_DIR "$ENV{HOME}/.cache/yetty/3rdparty")
    endif()
endif()

set(YETTY_3RDPARTY_TAG "3rdparty-${YETTY_3RDPARTY_VERSION}")
set(YETTY_3RDPARTY_OUTPUT_DIR "${CMAKE_BINARY_DIR}/3rdparty")

file(MAKE_DIRECTORY "${YETTY_3RDPARTY_CACHE_DIR}")
file(MAKE_DIRECTORY "${YETTY_3RDPARTY_OUTPUT_DIR}")

#-----------------------------------------------------------------------------
# yetty_3rdparty_target_platform(OUT_VAR)
#
# Maps the active build target to the <platform> slug used in the tarball
# filename. Must match the TARGET_PLATFORM values understood by every
# build-tools/3rdparty/<lib>/build.sh wrapper.
#-----------------------------------------------------------------------------
function(yetty_3rdparty_target_platform OUT_VAR)
    if(YETTY_ANDROID OR ANDROID)
        if(ANDROID_ABI STREQUAL "x86_64")
            set(_P "android-x86_64")
        else()
            set(_P "android-arm64-v8a")
        endif()
    elseif(YETTY_IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
        if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
            set(_P "ios-x86_64")
        else()
            set(_P "ios-arm64")
        endif()
    elseif(EMSCRIPTEN)
        set(_P "webasm")
    elseif(APPLE)
        if(CMAKE_OSX_ARCHITECTURES)
            set(_ARCHS "${CMAKE_OSX_ARCHITECTURES}")
        else()
            set(_ARCHS "${CMAKE_HOST_SYSTEM_PROCESSOR}")
        endif()
        if(_ARCHS MATCHES "x86_64")
            set(_P "macos-x86_64")
        else()
            set(_P "macos-arm64")
        endif()
    elseif(WIN32)
        set(_P "windows-x86_64")
    else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(_P "linux-aarch64")
        else()
            set(_P "linux-x86_64")
        endif()
    endif()
    set(${OUT_VAR} "${_P}" PARENT_SCOPE)
endfunction()

#-----------------------------------------------------------------------------
# yetty_3rdparty_fetch(LIB_NAME [DEST_VAR])
#
# Downloads ${URL_BASE}/${TAG}/<lib>-<platform>-<ver>.tar.gz into the cache,
# extracts into ${OUTPUT_DIR}/<lib>/. Sets DEST_VAR (if provided) to that
# directory in the caller's scope. Stamp-guarded: re-extracts only if the
# version changed.
#-----------------------------------------------------------------------------
function(yetty_3rdparty_fetch LIB_NAME)
    yetty_3rdparty_target_platform(_PLATFORM)
    if(NOT _PLATFORM)
        message(FATAL_ERROR
            "3rdparty-fetch(${LIB_NAME}): no target platform mapped — \
unsupported build configuration?")
    endif()

    set(_FILENAME "${LIB_NAME}-${_PLATFORM}-${YETTY_3RDPARTY_VERSION}.tar.gz")
    set(_TARBALL "${YETTY_3RDPARTY_CACHE_DIR}/${_FILENAME}")
    set(_URL "${YETTY_3RDPARTY_URL_BASE}/${YETTY_3RDPARTY_TAG}/${_FILENAME}")
    set(_DEST "${YETTY_3RDPARTY_OUTPUT_DIR}/${LIB_NAME}")
    set(_STAMP "${_DEST}/.fetched-${YETTY_3RDPARTY_VERSION}")

    if(NOT EXISTS "${_STAMP}")
        if(NOT EXISTS "${_TARBALL}")
            message(STATUS "3rdparty-fetch: downloading ${_FILENAME}")
            file(DOWNLOAD "${_URL}" "${_TARBALL}"
                SHOW_PROGRESS
                STATUS _DL_STATUS
                TLS_VERIFY ON
            )
            list(GET _DL_STATUS 0 _DL_CODE)
            if(NOT _DL_CODE EQUAL 0)
                file(REMOVE "${_TARBALL}")
                message(FATAL_ERROR
                    "3rdparty-fetch: download failed for ${_URL}: ${_DL_STATUS}")
            endif()
        endif()

        # Wipe stale extraction if version stamp is missing (re-extract clean)
        if(EXISTS "${_DEST}")
            file(REMOVE_RECURSE "${_DEST}")
        endif()
        file(MAKE_DIRECTORY "${_DEST}")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xzf "${_TARBALL}"
            WORKING_DIRECTORY "${_DEST}"
            RESULT_VARIABLE _TAR_RESULT
        )
        if(NOT _TAR_RESULT EQUAL 0)
            message(FATAL_ERROR
                "3rdparty-fetch: failed to extract ${_TARBALL} into ${_DEST}")
        endif()
        file(WRITE "${_STAMP}" "${YETTY_3RDPARTY_VERSION}\n")
        message(STATUS "3rdparty-fetch: ${LIB_NAME} ${YETTY_3RDPARTY_VERSION} ready (${_PLATFORM})")
    endif()

    if(ARGC GREATER 1)
        set(${ARGV1} "${_DEST}" PARENT_SCOPE)
    endif()
    set(YETTY_3RDPARTY_${LIB_NAME}_DIR "${_DEST}" CACHE INTERNAL "")
endfunction()
