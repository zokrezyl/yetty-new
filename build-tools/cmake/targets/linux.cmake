# Linux desktop build target

include(${YETTY_ROOT}/build-tools/cmake/targets/shared.cmake)

# Linux-specific libraries (guarded by variables.cmake)
if(YETTY_ENABLE_LIB_GLFW)
    include(${YETTY_ROOT}/build-tools/cmake/libs/glfw.cmake)
endif()
if(YETTY_ENABLE_LIB_LIBMAGIC)
    include(${YETTY_ROOT}/build-tools/cmake/Libmagic.cmake)
endif()

# TinyEMU - in-process RISC-V emulator for --temu flag
if(YETTY_ENABLE_LIB_TINYEMU)
    include(${YETTY_ROOT}/build-tools/cmake/tinyemu.cmake)
    include(${YETTY_ROOT}/build-tools/cmake/tinyemu-runtime.cmake)
endif()

# Shared RISC-V runtime: OpenSBI firmware, Linux kernel, Alpine rootfs.
# Needed by either --temu (TinyEMU) or --qemu (external QEMU via telnet).
if(YETTY_ENABLE_LIB_TINYEMU OR YETTY_ENABLE_LIB_QEMU)
    include(${YETTY_ROOT}/build-tools/cmake/opensbi.cmake)
    include(${YETTY_ROOT}/build-tools/cmake/linux-kernel.cmake)
    include(${YETTY_ROOT}/build-tools/cmake/alpine-rootfs.cmake)
endif()

# QEMU - external RISC-V emulator (accessed via telnet) for --qemu flag
if(YETTY_ENABLE_LIB_QEMU)
    include(${YETTY_ROOT}/build-tools/cmake/qemu.cmake)
endif()

# Desktop-specific subdirectories
if(YETTY_ENABLE_FEATURE_GPU)
    add_subdirectory(${YETTY_ROOT}/src/yetty/gpu ${CMAKE_BINARY_DIR}/src/yetty/gpu)
endif()
if(YETTY_ENABLE_FEATURE_CLIENT)
    add_subdirectory(${YETTY_ROOT}/src/yetty/client ${CMAKE_BINARY_DIR}/src/yetty/client)
endif()
if(YETTY_ENABLE_FEATURE_YTOP)
    add_subdirectory(${YETTY_ROOT}/src/yetty/ytop ${CMAKE_BINARY_DIR}/src/yetty/ytop)
endif()

# Platform sources — linux-specific + shared GLFW/Unix (C)
set(YETTY_PLATFORM_SOURCES
    ${YETTY_ROOT}/src/yetty/yplatform/shared/glfw-main.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/glfw-surface.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/glfw-event-loop.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/glfw-window.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/glfw-clipboard-manager.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/libuv-event-loop.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/fork-pty.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/unix-pty-factory.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/unix-pipe.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/extract-assets.c
    ${YETTY_ROOT}/src/yetty/incbin-assets.c
    ${YETTY_ROOT}/src/yetty/yplatform/linux/platform-paths.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/thread.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/term.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/fs.c
)

# TinyEMU PTY source (for --virtual flag)
if(YETTY_ENABLE_LIB_TINYEMU)
    list(APPEND YETTY_PLATFORM_SOURCES
        ${YETTY_ROOT}/src/yetty/yplatform/shared/tinyemu-pty.c
    )
endif()

# Create executable with core sources + platform
add_executable(yetty
    ${YETTY_SOURCES}
    ${YETTY_CORE_SOURCES}
    ${YETTY_DESKTOP_SOURCES}
    ${YETTY_PLATFORM_SOURCES}
)

target_include_directories(yetty PRIVATE ${YETTY_INCLUDES} ${YETTY_RENDERER_INCLUDES} ${JPEG_INCLUDE_DIRS} ${BROTLI_INCLUDE_DIR})

# Build shared RISC-V runtime (OpenSBI, Linux kernel, Alpine rootfs) BEFORE
# embedding assets. These live in ${CMAKE_BINARY_DIR}/assets/yemu and are
# shared between --temu and --qemu modes. The .cfg is created at runtime in
# the user config dir (see tinyemu-pty.c), not here.
if(YETTY_ENABLE_LIB_TINYEMU OR YETTY_ENABLE_LIB_QEMU)
    opensbi_build()
    linux_kernel_build()
    alpine_rootfs_download()
    alpine_rootfs_create_image()
endif()

# QEMU: Build at configure time BEFORE embedding assets
if(YETTY_ENABLE_LIB_QEMU)
    qemu_build()
endif()

# Embed all assets (logo, shaders, fonts, CDB files, TinyEMU)
yetty_embed_assets(yetty)

# Dummy targets for dependency tracking (legacy)
add_custom_target(copy-shaders-for-incbin)
add_custom_target(copy-fonts-for-incbin)

target_compile_definitions(yetty PRIVATE
    ${YETTY_DEFINITIONS}
    YETTY_WEB=0
    YETTY_ANDROID=0
    YETTY_USE_PREBUILT_ATLAS=0
    YETTY_USE_FONTCONFIG=1
    YETTY_USE_FORKPTY=1
    YETTY_HAS_VNC=1
    $<$<BOOL:${YETTY_ENABLE_LIB_TINYEMU}>:YETTY_HAS_TINYEMU=1>
    $<$<BOOL:${YETTY_ENABLE_LIB_TINYEMU}>:CONFIG_SLIRP>
    $<$<BOOL:${YETTY_ENABLE_LIB_QEMU}>:YETTY_HAS_QEMU=1>
)

set_target_properties(yetty PROPERTIES ENABLE_EXPORTS TRUE)

# Fontconfig linking
find_package(PkgConfig REQUIRED)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
target_include_directories(yetty PRIVATE ${FONTCONFIG_INCLUDE_DIRS})

if(DEFINED ENV{NIX_CC})
    # Nix: use static libs from env vars set by flake.nix
    set(FONTCONFIG_STATIC_LIB "$ENV{FONTCONFIG_STATIC_LIB}")
    set(EXPAT_STATIC_LIB "$ENV{EXPAT_STATIC_LIB}")
    set(UUID_STATIC_LIB "$ENV{UUID_STATIC_LIB}")
    message(STATUS "Nix detected - using static fontconfig: ${FONTCONFIG_STATIC_LIB}")
    set(FONTCONFIG_LINK_LIBS ${FONTCONFIG_STATIC_LIB} ${EXPAT_STATIC_LIB} ${UUID_STATIC_LIB})
else()
    # System: use static libs
    find_library(FONTCONFIG_STATIC_LIB libfontconfig.a PATHS /usr/lib/x86_64-linux-gnu /usr/lib NO_DEFAULT_PATH REQUIRED)
    find_library(EXPAT_STATIC_LIB libexpat.a PATHS /usr/lib/x86_64-linux-gnu /usr/lib NO_DEFAULT_PATH REQUIRED)
    find_library(UUID_STATIC_LIB libuuid.a PATHS /usr/lib/x86_64-linux-gnu /usr/lib NO_DEFAULT_PATH REQUIRED)
    message(STATUS "Using static fontconfig/expat/uuid")
    set(FONTCONFIG_LINK_LIBS ${FONTCONFIG_STATIC_LIB} ${EXPAT_STATIC_LIB} ${UUID_STATIC_LIB})
endif()

target_link_libraries(yetty PRIVATE
    ${YETTY_LIBS}
    ${BROTLIDEC_LIBRARIES}
    ${FONTCONFIG_LINK_LIBS}
    $<$<BOOL:${YETTY_ENABLE_LIB_TINYEMU}>:tinyemu>
    $<$<BOOL:${YETTY_ENABLE_LIB_QEMU}>:yetty_qemu>
    yetty_telnet
    rt
    util
)

# CDB font generation
if(YETTY_ENABLE_FEATURE_CDB_GEN)
    include(${YETTY_ROOT}/build-tools/cmake/cdb-gen.cmake)
endif()

# Copy runtime assets to build directory
if(YETTY_ENABLE_FEATURE_ASSETS)
    add_subdirectory(${YETTY_ROOT}/assets ${CMAKE_BINARY_DIR}/assets-build)
endif()

# Ensure all runtime assets are in build output before yetty
if(YETTY_ENABLE_FEATURE_ASSETS)
    add_dependencies(yetty copy-shaders copy-assets copy-shaders-for-incbin copy-fonts-for-incbin)
endif()
if(YETTY_ENABLE_FEATURE_CDB_GEN)
    add_dependencies(yetty generate-cdb)
endif()

# Verify all required assets are present
if(YETTY_ENABLE_FEATURE_ASSETS)
    add_custom_command(TARGET yetty POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DTARGET_TYPE=desktop -DCHECK_CDB=${YETTY_ENABLE_FEATURE_CDB_GEN} -P ${YETTY_ROOT}/build-tools/cmake/verify-assets.cmake
        COMMENT "Verifying build assets..."
    )
endif()

