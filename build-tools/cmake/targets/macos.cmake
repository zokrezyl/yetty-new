# macOS desktop build target

include(${YETTY_ROOT}/build-tools/cmake/targets/shared.cmake)

# macOS-specific libraries (guarded by variables.cmake)
if(YETTY_ENABLE_LIB_GLFW)
    include(${YETTY_ROOT}/build-tools/cmake/libs/glfw.cmake)
endif()
if(YETTY_ENABLE_LIB_LIBMAGIC)
    include(${YETTY_ROOT}/build-tools/cmake/Libmagic.cmake)
endif()

# Add src/yetty (builds libraries)
add_subdirectory(${YETTY_ROOT}/src/yetty ${CMAKE_BINARY_DIR}/src/yetty)

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

# Platform manager sources (new architecture)
# Note: event-loop is included via yetty_base
set(YETTY_PLATFORM_SOURCES
    ${YETTY_ROOT}/src/yetty/platform/init-manager/glfw.cpp
    ${YETTY_ROOT}/src/yetty/platform/shared/glfw-window-singleton.cpp
    ${YETTY_ROOT}/src/yetty/platform/surface-manager/glfw.cpp
    ${YETTY_ROOT}/src/yetty/platform/pty-manager/unix.cpp
    ${YETTY_ROOT}/src/yetty/platform/pty-reader/unix.cpp
    ${YETTY_ROOT}/src/yetty/platform/fs-path-manager/unix.cpp
    ${YETTY_ROOT}/src/yetty/platform/clipboard-manager/glfw.cpp
    ${YETTY_ROOT}/src/yetty/platform/webgpu-manager/macos.cpp
)

# Create executable with core sources + platform
add_executable(yetty
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
    YETTY_USE_CORETEXT=1
    YETTY_USE_FORKPTY=1
    YETTY_HAS_VNC=1
)

set_target_properties(yetty PROPERTIES ENABLE_EXPORTS TRUE)

# Core Text for font discovery
find_library(CORETEXT_LIBRARY CoreText REQUIRED)
find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)

target_link_libraries(yetty PRIVATE
    ${YETTY_LIBS}
    ${CORETEXT_LIBRARY}
    ${COREFOUNDATION_LIBRARY}
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
if(YETTY_ENABLE_FEATURE_CDB_GEN AND YETTY_ENABLE_FEATURE_ASSETS)
    add_dependencies(yetty generate-cdb copy-shaders copy-assets copy-shaders-for-incbin copy-fonts-for-incbin)
endif()

# Verify all required assets are present
if(YETTY_ENABLE_FEATURE_ASSETS)
    add_custom_command(TARGET yetty POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DTARGET_TYPE=desktop -P ${YETTY_ROOT}/build-tools/cmake/verify-assets.cmake
        COMMENT "Verifying build assets..."
    )
endif()
