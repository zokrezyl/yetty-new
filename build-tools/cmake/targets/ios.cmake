# iOS build target

# Disable desktop-only libraries
set(YETTY_ENABLE_LIB_GLFW OFF CACHE BOOL "" FORCE)

include(${YETTY_ROOT}/build-tools/cmake/targets/shared.cmake)

# Set iOS assets directory
set(IOS_ASSETS_DIR "${CMAKE_BINARY_DIR}/ios-assets")
file(MAKE_DIRECTORY ${IOS_ASSETS_DIR})

# Platform sources — iOS-specific (Objective-C) + shared Unix (C)
set(YETTY_PLATFORM_SOURCES
    ${YETTY_ROOT}/src/yetty/platform/ios/main.m
    ${YETTY_ROOT}/src/yetty/platform/ios/platform-paths.m
    ${YETTY_ROOT}/src/yetty/platform/ios/surface.m
    ${YETTY_ROOT}/src/yetty/platform/shared/libuv-event-loop.c
    ${YETTY_ROOT}/src/yetty/platform/shared/unix-pipe.c
    ${YETTY_ROOT}/src/yetty/platform/shared/unix-pty.c
    ${YETTY_ROOT}/src/yetty/platform/shared/extract-assets.c
    ${YETTY_ROOT}/src/yetty/incbin-assets.c
)

# Create iOS app bundle
add_executable(yetty MACOSX_BUNDLE
    ${YETTY_SOURCES}
    ${YETTY_CORE_SOURCES}
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
    YETTY_IOS=1
    YETTY_USE_PREBUILT_ATLAS=1
    YETTY_ASSETS_FROM_BUNDLE=1
    YETTY_USE_CORETEXT=1
    YETTY_USE_FORKPTY=0
    YETTY_HAS_VNC=1
    YETTY_HAS_YVIDEO=1
)

# iOS app bundle properties
set_target_properties(yetty PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.yetty.terminal"
    MACOSX_BUNDLE_BUNDLE_NAME "Yetty"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    MACOSX_BUNDLE_INFO_PLIST "${YETTY_ROOT}/build-tools/ios/Info.plist"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
)

# Apple frameworks for iOS
find_library(UIKIT_LIBRARY UIKit REQUIRED)
find_library(CORETEXT_LIBRARY CoreText REQUIRED)
find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
find_library(COREGRAPHICS_LIBRARY CoreGraphics REQUIRED)
find_library(METAL_LIBRARY Metal REQUIRED)
find_library(QUARTZCORE_LIBRARY QuartzCore REQUIRED)

target_link_libraries(yetty PRIVATE
    ${YETTY_LIBS}
    ${CORETEXT_LIBRARY}
    ${COREFOUNDATION_LIBRARY}
    ${COREGRAPHICS_LIBRARY}
    ${UIKIT_LIBRARY}
    ${METAL_LIBRARY}
    ${QUARTZCORE_LIBRARY}
)

# CDB font generation (builds host tools for cross-compilation)
if(YETTY_ENABLE_FEATURE_CDB_GEN)
    include(${YETTY_ROOT}/build-tools/cmake/cdb-gen.cmake)
endif()

# Generate demo outputs (pre-run demo scripts to capture output for iOS)
if(YETTY_ENABLE_FEATURE_DEMO)
    message(STATUS "Generating demo outputs for iOS...")
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -DYETTY_ROOT=${YETTY_ROOT}
            -DOUTPUT_DIR=${IOS_ASSETS_DIR}
            -P ${YETTY_ROOT}/build-tools/cmake/generate-demo-outputs.cmake
        WORKING_DIRECTORY ${YETTY_ROOT}
    )
endif()

# Copy static assets to iOS build directory
if(YETTY_ENABLE_FEATURE_ASSETS)
    file(GLOB ASSET_FILES "${YETTY_ROOT}/assets/*")
    file(COPY ${ASSET_FILES} DESTINATION ${IOS_ASSETS_DIR})
endif()

# Ensure CDB, shaders, and assets are built before yetty
if(YETTY_ENABLE_FEATURE_CDB_GEN)
    add_dependencies(yetty generate-cdb copy-shaders copy-shaders-for-incbin copy-fonts-for-incbin)
endif()

# Copy generated CDB fonts and shaders to iOS assets dir after build
if(YETTY_ENABLE_FEATURE_CDB_GEN)
    add_custom_command(TARGET yetty POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/assets/msdf-fonts ${IOS_ASSETS_DIR}/msdf-fonts
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/assets/shaders ${IOS_ASSETS_DIR}/shaders
        COMMENT "Copying CDB fonts and shaders to iOS assets"
    )
endif()

# Verify all required assets are present
if(YETTY_ENABLE_FEATURE_ASSETS)
    add_custom_command(TARGET yetty POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${IOS_ASSETS_DIR}/.. -DTARGET_TYPE=ios -P ${YETTY_ROOT}/build-tools/cmake/verify-assets.cmake
        COMMENT "Verifying build assets..."
    )
endif()
