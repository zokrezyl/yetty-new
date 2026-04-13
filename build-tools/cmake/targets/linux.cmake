# Linux desktop build target

include(${YETTY_ROOT}/build-tools/cmake/targets/shared.cmake)

# Linux-specific libraries (guarded by variables.cmake)
if(YETTY_ENABLE_LIB_GLFW)
    include(${YETTY_ROOT}/build-tools/cmake/libs/glfw.cmake)
endif()
if(YETTY_ENABLE_LIB_LIBMAGIC)
    include(${YETTY_ROOT}/build-tools/cmake/Libmagic.cmake)
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
    ${YETTY_ROOT}/src/yetty/yplatform/shared/unix-pty.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/unix-pipe.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/extract-assets.c
    ${YETTY_ROOT}/src/yetty/incbin-assets.c
    ${YETTY_ROOT}/src/yetty/yplatform/linux/platform-paths.c
)

# Create executable with core sources + platform
add_executable(yetty
    ${YETTY_SOURCES}
    ${YETTY_CORE_SOURCES}
    ${YETTY_DESKTOP_SOURCES}
    ${YETTY_PLATFORM_SOURCES}
)

target_include_directories(yetty PRIVATE ${YETTY_INCLUDES} ${YETTY_RENDERER_INCLUDES} ${JPEG_INCLUDE_DIRS} ${BROTLI_INCLUDE_DIR})

# Embed all assets (logo, shaders, fonts, CDB files)
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
)

set_target_properties(yetty PROPERTIES ENABLE_EXPORTS TRUE)

# Fontconfig
find_package(PkgConfig REQUIRED)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
target_include_directories(yetty PRIVATE ${FONTCONFIG_INCLUDE_DIRS})
find_library(FONTCONFIG_STATIC_LIB libfontconfig.a PATHS /usr/lib/x86_64-linux-gnu /usr/lib REQUIRED)
find_library(EXPAT_STATIC_LIB libexpat.a PATHS /usr/lib/x86_64-linux-gnu /usr/lib REQUIRED)
find_library(UUID_STATIC_LIB libuuid.a PATHS /usr/lib/x86_64-linux-gnu /usr/lib REQUIRED)

target_link_libraries(yetty PRIVATE
    ${YETTY_LIBS}
    ${BROTLIDEC_LIBRARIES}
    ${FONTCONFIG_STATIC_LIB}
    ${EXPAT_STATIC_LIB}
    ${UUID_STATIC_LIB}
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

# Tests
if(YETTY_ENABLE_FEATURE_TESTS)
    enable_testing()
    add_subdirectory(${YETTY_ROOT}/test/ut ${CMAKE_BINARY_DIR}/test/ut)
endif()

# Tools (ydraw-maze, ydraw-zoo, etc.)
if(YETTY_ENABLE_FEATURE_TOOLS)
    add_subdirectory(${YETTY_ROOT}/tools ${CMAKE_BINARY_DIR}/tools)
endif()

# Demos
if(YETTY_ENABLE_FEATURE_DEMO)
    add_subdirectory(${YETTY_ROOT}/demo ${CMAKE_BINARY_DIR}/demo)
endif()
