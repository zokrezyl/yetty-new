# Android build target

include(${YETTY_ROOT}/build-tools/cmake/targets/shared.cmake)

# native_app_glue from Android NDK
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue
)

# Toybox configuration
if(TOYBOX_PATH)
    add_compile_definitions(YETTY_TOYBOX_PATH="${TOYBOX_PATH}")
endif()

# Set ANDROID_ASSETS_DIR BEFORE adding yetty subdirectory so shaders/CMakeLists.txt can use it
set(ANDROID_ASSETS_DIR "${ANDROID_BUILD_DIR}/assets")
file(MAKE_DIRECTORY ${ANDROID_ASSETS_DIR})

# Add src/yetty (builds libraries)
add_subdirectory(${YETTY_ROOT}/src/yetty ${CMAKE_BINARY_DIR}/src/yetty)

# VNC server/client support
if(YETTY_ENABLE_FEATURE_VNC)
    add_subdirectory(${YETTY_ROOT}/src/yetty/vnc ${CMAKE_BINARY_DIR}/src/yetty/vnc)
endif()

# Platform sources — Android-specific + shared Unix components
set(YETTY_PLATFORM_SOURCES
    ${YETTY_ROOT}/src/yetty/platform/android/main.cpp
    ${YETTY_ROOT}/src/yetty/platform/android/platform-paths.cpp
    ${YETTY_ROOT}/src/yetty/platform/android/surface.cpp
    ${YETTY_ROOT}/src/yetty/platform/shared/libuv-event-loop.cpp
    ${YETTY_ROOT}/src/yetty/platform/shared/unix-pipe.cpp
    ${YETTY_ROOT}/src/yetty/platform/shared/unix-pty.cpp
)

# Create shared library with core sources + android platform
add_library(yetty SHARED
    ${YETTY_CORE_SOURCES}
    ${YETTY_ANDROID_SOURCES}
    ${YETTY_PLATFORM_SOURCES}
    ${YETTY_ROOT}/src/yetty/msdf-gen/generator.cpp
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
    YETTY_ANDROID=1
    YETTY_USE_PREBUILT_ATLAS=1
    YETTY_ASSETS_FROM_APK=1
    YETTY_HAS_VNC=1
)

set_target_properties(yetty PROPERTIES LIBRARY_OUTPUT_NAME "yetty")

target_link_libraries(yetty PRIVATE
    ${YETTY_LIBS}
    -Wl,--whole-archive
    native_app_glue
    -Wl,--no-whole-archive
    android
    log
)

# CDB font generation (builds host tools for cross-compilation)
if(YETTY_ENABLE_FEATURE_CDB_GEN)
    include(${YETTY_ROOT}/build-tools/cmake/cdb-gen.cmake)
endif()

# Generate demo outputs (pre-run demo scripts to capture output for Android)
if(YETTY_ENABLE_FEATURE_DEMO)
    message(STATUS "Generating demo outputs for Android...")
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -DYETTY_ROOT=${YETTY_ROOT}
            -DOUTPUT_DIR=${ANDROID_ASSETS_DIR}
            -P ${YETTY_ROOT}/build-tools/cmake/generate-demo-outputs.cmake
        WORKING_DIRECTORY ${YETTY_ROOT}
    )
endif()

# Copy static assets to Android build directory
if(YETTY_ENABLE_FEATURE_ASSETS)
    file(GLOB ASSET_FILES "${YETTY_ROOT}/assets/*")
    file(COPY ${ASSET_FILES} DESTINATION ${ANDROID_ASSETS_DIR})
endif()

# Ensure CDB, shaders, and assets are built before yetty
if(YETTY_ENABLE_FEATURE_CDB_GEN)
    add_dependencies(yetty generate-cdb copy-shaders copy-shaders-for-incbin copy-fonts-for-incbin)
endif()

# Copy generated CDB fonts to Android assets dir after build
if(YETTY_ENABLE_FEATURE_CDB_GEN)
    add_custom_command(TARGET yetty POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/assets/msdf-fonts ${ANDROID_ASSETS_DIR}/msdf-fonts
        COMMENT "Copying CDB fonts to Android assets"
    )
endif()

# Verify all required assets are present
if(YETTY_ENABLE_FEATURE_ASSETS)
    add_custom_command(TARGET yetty POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${ANDROID_ASSETS_DIR}/.. -DTARGET_TYPE=android -P ${YETTY_ROOT}/build-tools/cmake/verify-assets.cmake
        COMMENT "Verifying build assets..."
    )
endif()
