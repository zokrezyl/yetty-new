# assets-fetch.cmake
#
# Downloads the prebuilt asset tarballs published by build-assets.yml
# (GitHub release tag `assets-${YETTY_ASSETS_VERSION}`) and extracts them
# into the build tree so the rest of the build can consume them exactly
# as if they had been produced locally.
#
# Output layout (consumed by targets/shared.cmake::yetty_embed_assets):
#   ${CMAKE_BINARY_DIR}/assets/yemu/
#       kernel-riscv64.bin
#       opensbi-fw_jump.elf
#       opensbi-fw_dynamic.bin
#       alpine-rootfs.img       (raw ext4, virtio-blk root for tinyemu)
#       alpine-rootfs/...       (still bundled in the linux asset, used by --qemu 9p)
#   ${CMAKE_BINARY_DIR}/assets/qemu/
#       qemu-system-riscv64[.exe]
#   ${CMAKE_BINARY_DIR}/assets/msdf-fonts/
#       DejaVuSansMNerdFontMono-{Regular,Bold,Oblique,BoldOblique}.cdb
#
# Pinning: version is read from ${YETTY_ROOT}/assets.version (bumped when
# you cut a new assets-X.Y.Z release). Overrides via CMake/env var
# YETTY_ASSETS_VERSION.
#
# URL override: YETTY_ASSETS_URL_BASE (default: GitHub releases for the
# upstream repo).

include_guard(GLOBAL)

#-----------------------------------------------------------------------------
# Resolve version + url base
#-----------------------------------------------------------------------------
if(NOT DEFINED YETTY_ASSETS_VERSION)
    if(DEFINED ENV{YETTY_ASSETS_VERSION})
        set(YETTY_ASSETS_VERSION "$ENV{YETTY_ASSETS_VERSION}")
    else()
        set(_VER_FILE "${YETTY_ROOT}/assets.version")
        if(NOT EXISTS "${_VER_FILE}")
            message(FATAL_ERROR "assets-fetch: ${_VER_FILE} not found")
        endif()
        file(READ "${_VER_FILE}" YETTY_ASSETS_VERSION)
        string(STRIP "${YETTY_ASSETS_VERSION}" YETTY_ASSETS_VERSION)
    endif()
endif()
set(YETTY_ASSETS_VERSION "${YETTY_ASSETS_VERSION}"
    CACHE STRING "Pinned asset release version")

if(NOT DEFINED YETTY_ASSETS_URL_BASE)
    if(DEFINED ENV{YETTY_ASSETS_URL_BASE})
        set(YETTY_ASSETS_URL_BASE "$ENV{YETTY_ASSETS_URL_BASE}")
    else()
        set(YETTY_ASSETS_URL_BASE "https://github.com/zokrezyl/yetty/releases/download")
    endif()
endif()
set(YETTY_ASSETS_URL_BASE "${YETTY_ASSETS_URL_BASE}"
    CACHE STRING "Base URL for asset downloads")

if(NOT DEFINED YETTY_ASSETS_CACHE_DIR)
    if(DEFINED ENV{YETTY_ASSETS_CACHE_DIR})
        set(YETTY_ASSETS_CACHE_DIR "$ENV{YETTY_ASSETS_CACHE_DIR}")
    else()
        # Cache across builds (not wiped when build dir is cleaned)
        set(YETTY_ASSETS_CACHE_DIR "$ENV{HOME}/.cache/yetty/assets")
    endif()
endif()

set(YETTY_ASSETS_TAG "assets-${YETTY_ASSETS_VERSION}")
set(YETTY_ASSETS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/assets")

file(MAKE_DIRECTORY "${YETTY_ASSETS_CACHE_DIR}")
file(MAKE_DIRECTORY "${YETTY_ASSETS_OUTPUT_DIR}")

#-----------------------------------------------------------------------------
# _yetty_assets_fetch_one(asset_name extract_subdir stamp_file)
#
# Downloads ${URL_BASE}/${TAG}/${asset_name}-${V}.tar.gz into the cache,
# then extracts into ${OUTPUT_DIR}/${extract_subdir}. stamp_file gates
# re-extraction across configure runs.
#-----------------------------------------------------------------------------
function(_yetty_assets_fetch_one ASSET_NAME EXTRACT_SUBDIR STAMP_FILE)
    set(_FILENAME "${ASSET_NAME}-${YETTY_ASSETS_VERSION}.tar.gz")
    set(_TARBALL "${YETTY_ASSETS_CACHE_DIR}/${_FILENAME}")
    set(_URL "${YETTY_ASSETS_URL_BASE}/${YETTY_ASSETS_TAG}/${_FILENAME}")
    set(_DEST "${YETTY_ASSETS_OUTPUT_DIR}/${EXTRACT_SUBDIR}")
    set(_STAMP "${_DEST}/${STAMP_FILE}")

    if(EXISTS "${_STAMP}")
        return()
    endif()

    if(NOT EXISTS "${_TARBALL}")
        message(STATUS "assets-fetch: downloading ${_FILENAME}")
        file(DOWNLOAD "${_URL}" "${_TARBALL}"
            SHOW_PROGRESS
            STATUS _DL_STATUS
            TLS_VERIFY ON
        )
        list(GET _DL_STATUS 0 _DL_CODE)
        if(NOT _DL_CODE EQUAL 0)
            file(REMOVE "${_TARBALL}")
            message(FATAL_ERROR
                "assets-fetch: download failed for ${_URL}: ${_DL_STATUS}")
        endif()
    endif()

    file(MAKE_DIRECTORY "${_DEST}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf "${_TARBALL}"
        WORKING_DIRECTORY "${_DEST}"
        RESULT_VARIABLE _TAR_RESULT
    )
    if(NOT _TAR_RESULT EQUAL 0)
        message(FATAL_ERROR
            "assets-fetch: failed to extract ${_TARBALL} into ${_DEST}")
    endif()

    file(WRITE "${_STAMP}" "${YETTY_ASSETS_VERSION}\n")
endfunction()

#-----------------------------------------------------------------------------
# _yetty_assets_qemu_platform(OUT_VAR)
#
# Maps the active yetty target to the qemu-<platform> asset slug. The QEMU
# binary is embedded into the app and runs on the end user's machine, so
# the slug reflects the target platform, not the cmake host.
#-----------------------------------------------------------------------------
function(_yetty_assets_qemu_platform OUT_VAR)
    if(YETTY_ANDROID)
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
    elseif(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
        set(_P "tvos-arm64")
    elseif(EMSCRIPTEN)
        # webasm uses in-process tinyemu, no prebuilt qemu binary
        set(_P "")
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
# assets_fetch_cdb()        — CDB font databases (platform-independent)
# assets_fetch_yemu()       — OpenSBI + Linux kernel + Alpine rootfs
# assets_fetch_qemu()       — QEMU binary for the active target platform
#
# Each is idempotent across configure runs (stamp-guarded).
#-----------------------------------------------------------------------------
function(assets_fetch_cdb)
    message(STATUS "assets-fetch: cdb-${YETTY_ASSETS_VERSION}")
    _yetty_assets_fetch_one("cdb" "msdf-fonts" ".fetched")
    # Tarball ships .cdb.br (brotli-pre-compressed at q11). The embed
    # pipeline (incbin.cmake) detects the .br suffix and embeds the bytes
    # as-is — no decompress, no recompress. Pure passthrough.
endfunction()

# Decompress a single .br file to its raw form alongside, idempotently.
# Used for assets that need both modes:
#   - embed pipeline (incbin) reads the .br directly (already-compressed path)
#   - runtime path mode (tinyemu_copy_runtime_to_bundle) reads the raw file
function(_yetty_assets_decompress_br BR_FILE)
    string(REGEX REPLACE "\\.br$" "" _RAW "${BR_FILE}")
    if(EXISTS "${_RAW}" AND EXISTS "${BR_FILE}")
        # Already decompressed; check timestamp to allow re-run after a
        # fresh download.
        file(TIMESTAMP "${BR_FILE}"  _T_BR)
        file(TIMESTAMP "${_RAW}"     _T_RAW)
        if(_T_RAW STREQUAL _T_BR OR NOT _T_RAW STRLESS _T_BR)
            return()
        endif()
    endif()
    if(NOT EXISTS "${BR_FILE}")
        return()
    endif()
    find_program(BROTLI_EXECUTABLE brotli)
    if(NOT BROTLI_EXECUTABLE)
        message(FATAL_ERROR
            "assets-fetch: brotli not found; needed to decompress ${BR_FILE}")
    endif()
    execute_process(
        COMMAND "${BROTLI_EXECUTABLE}" -d -f -o "${_RAW}" "${BR_FILE}"
        RESULT_VARIABLE _R
    )
    if(NOT _R EQUAL 0)
        message(FATAL_ERROR "assets-fetch: failed to decompress ${BR_FILE}")
    endif()
endfunction()

function(assets_fetch_yemu)
    message(STATUS "assets-fetch: opensbi-${YETTY_ASSETS_VERSION}")
    _yetty_assets_fetch_one("opensbi" "yemu" ".fetched-opensbi")
    message(STATUS "assets-fetch: linux-${YETTY_ASSETS_VERSION}")
    _yetty_assets_fetch_one("linux"   "yemu" ".fetched-linux")
    message(STATUS "assets-fetch: alpine-disk-${YETTY_ASSETS_VERSION}")
    _yetty_assets_fetch_one("alpine-disk" "yemu" ".fetched-alpine-disk")

    # Producer ships .br only (q11). Decompress side-by-side here so
    # runtime path mode (tinyemu_copy_runtime_to_bundle, verify-assets)
    # finds the raw files. Embed pipeline picks the .br directly via
    # incbin's already-compressed path.
    set(_YEMU "${YETTY_ASSETS_OUTPUT_DIR}/yemu")
    _yetty_assets_decompress_br("${_YEMU}/opensbi-fw_jump.elf.br")
    _yetty_assets_decompress_br("${_YEMU}/opensbi-fw_dynamic.bin.br")
    _yetty_assets_decompress_br("${_YEMU}/kernel-riscv64.bin.br")
    _yetty_assets_decompress_br("${_YEMU}/alpine-rootfs.img.br")

    set(TINYEMU_OPENSBI_PATH
        "${_YEMU}/opensbi-fw_jump.elf"
        CACHE FILEPATH "" FORCE)
    set(QEMU_OPENSBI_PATH
        "${_YEMU}/opensbi-fw_dynamic.bin"
        CACHE FILEPATH "" FORCE)
    set(TINYEMU_KERNEL_PATH
        "${_YEMU}/kernel-riscv64.bin"
        CACHE FILEPATH "" FORCE)
    set(TINYEMU_ROOTFS_IMG
        "${_YEMU}/alpine-rootfs.img"
        CACHE FILEPATH "" FORCE)
endfunction()

function(assets_fetch_qemu)
    _yetty_assets_qemu_platform(_P)
    if(_P STREQUAL "")
        return()   # webasm — no qemu asset
    endif()

    set(_QEMU_DIR "${YETTY_ASSETS_OUTPUT_DIR}/qemu")

    if(WIN32)
        # TEMPORARY: Windows uses the locally-built qemu from
        # poc/qemu/build-tools/build-windows-minimal.ps1 instead of the
        # published asset tarball, until we add a Windows qemu build to the
        # release pipeline. The .ps1 must have been run before configure.
        set(_LOCAL_DIR "${YETTY_ROOT}/build-windows-minimal")
        set(_LOCAL_EXE "${_LOCAL_DIR}/qemu-system-riscv64.exe")
        if(NOT EXISTS "${_LOCAL_EXE}")
            message(FATAL_ERROR
                "assets-fetch: ${_LOCAL_EXE} not found. "
                "Run poc/qemu/build-tools/build-windows-minimal.ps1 first.")
        endif()
        message(STATUS "assets-fetch: using local qemu at ${_LOCAL_EXE}")
        file(MAKE_DIRECTORY "${_QEMU_DIR}")
        file(COPY "${_LOCAL_EXE}" DESTINATION "${_QEMU_DIR}")
        file(GLOB _DLLS "${_LOCAL_DIR}/*.dll")
        foreach(_D ${_DLLS})
            file(COPY "${_D}" DESTINATION "${_QEMU_DIR}")
        endforeach()
    else()
        message(STATUS "assets-fetch: qemu-${_P}-${YETTY_ASSETS_VERSION}")
        _yetty_assets_fetch_one("qemu-${_P}" "qemu" ".fetched-${_P}")
    endif()

    if(EXISTS "${_QEMU_DIR}/qemu-system-riscv64.exe")
        set(YETTY_QEMU_BINARY "${_QEMU_DIR}/qemu-system-riscv64.exe"
            CACHE FILEPATH "" FORCE)
    else()
        set(YETTY_QEMU_BINARY "${_QEMU_DIR}/qemu-system-riscv64"
            CACHE FILEPATH "" FORCE)
    endif()
    set(QEMU_OUTPUT_DIR "${_QEMU_DIR}" CACHE PATH "" FORCE)
endfunction()

#-----------------------------------------------------------------------------
# qemu_embed_runtime(target)
#
# Embeds the prebuilt qemu-system-riscv64 binary into the given target via
# incbin under the "qemu" asset prefix. Assumes assets_fetch_qemu() already
# populated ${QEMU_OUTPUT_DIR}.
#-----------------------------------------------------------------------------
function(qemu_embed_runtime TARGET)
    if(NOT COMMAND incbin_add_directory)
        include(${YETTY_ROOT}/build-tools/cmake/incbin.cmake)
    endif()

    set(_STAGING "${CMAKE_BINARY_DIR}/incbin-qemu")
    file(MAKE_DIRECTORY "${_STAGING}")

    if(EXISTS "${YETTY_QEMU_BINARY}")
        file(COPY "${YETTY_QEMU_BINARY}" DESTINATION "${_STAGING}")
    else()
        message(WARNING "qemu_embed_runtime: ${YETTY_QEMU_BINARY} not found; QEMU will be embedded empty")
    endif()

    # Not compressed — it's an executable, runtime extracts and chmods it
    incbin_add_directory(${TARGET} "qemu" "${_STAGING}" "*" FALSE)
endfunction()
