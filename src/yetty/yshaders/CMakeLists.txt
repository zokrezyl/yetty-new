# Copy all shaders to build directory
set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SHADER_DST_DIR ${CMAKE_BINARY_DIR}/assets/shaders)

# Collect all shader files recursively
file(GLOB_RECURSE SHADER_FILES
    ${SHADER_SRC_DIR}/*.wgsl
)

# Module shaders (embedded via incbin on native, loaded from assets on webasm)
set(MODULE_SHADERS
    ${YETTY_ROOT}/src/yetty/yfont/ms-raster-font.wgsl
    ${YETTY_ROOT}/src/yetty/yfont/raster-font.wgsl
    ${YETTY_ROOT}/src/yetty/yfont/ms-msdf-font.wgsl
    ${YETTY_ROOT}/src/yetty/yfont/msdf-font.wgsl
    ${YETTY_ROOT}/src/yetty/yterm/text-layer.wgsl
    ${YETTY_ROOT}/src/yetty/yterm/ypaint-layer.wgsl
    ${YETTY_ROOT}/src/yetty/yrender/blend.wgsl
)

# For Android: copy shaders at configure time to ANDROID_ASSETS_DIR
# This ensures shaders are in place BEFORE gradle packages the APK
if(ANDROID_ASSETS_DIR)
    file(MAKE_DIRECTORY ${ANDROID_ASSETS_DIR}/shaders)
    foreach(SHADER_FILE ${SHADER_FILES})
        file(RELATIVE_PATH REL_PATH ${SHADER_SRC_DIR} ${SHADER_FILE})
        get_filename_component(REL_DIR ${REL_PATH} DIRECTORY)
        if(REL_DIR)
            file(MAKE_DIRECTORY ${ANDROID_ASSETS_DIR}/shaders/${REL_DIR})
        endif()
        file(COPY ${SHADER_FILE} DESTINATION ${ANDROID_ASSETS_DIR}/shaders/${REL_DIR})
    endforeach()
    # Copy module shaders
    foreach(SHADER_FILE ${MODULE_SHADERS})
        file(COPY ${SHADER_FILE} DESTINATION ${ANDROID_ASSETS_DIR}/shaders)
    endforeach()
    # Also copy gpu-screen.wgsl to root for backward compat
    file(COPY ${SHADER_SRC_DIR}/gpu-screen.wgsl DESTINATION ${ANDROID_ASSETS_DIR})
    message(STATUS "Copied shaders to Android assets: ${ANDROID_ASSETS_DIR}/shaders")
endif()

# Create custom target to copy shaders (for non-Android builds and incremental builds)
add_custom_target(copy-shaders ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DST_DIR}
    COMMENT "Copying shaders to build directory"
)

foreach(SHADER_FILE ${SHADER_FILES})
    # Get relative path from shader source dir
    file(RELATIVE_PATH REL_PATH ${SHADER_SRC_DIR} ${SHADER_FILE})
    get_filename_component(REL_DIR ${REL_PATH} DIRECTORY)

    add_custom_command(TARGET copy-shaders POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DST_DIR}/${REL_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHADER_FILE} ${SHADER_DST_DIR}/${REL_PATH}
    )
endforeach()

# Also copy the main gpu-screen.wgsl to root for backward compat
add_custom_command(TARGET copy-shaders POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHADER_SRC_DIR}/gpu-screen.wgsl ${CMAKE_BINARY_DIR}/assets/gpu-screen.wgsl
)

# Copy module shaders to assets/shaders (needed for webasm)
foreach(SHADER_FILE ${MODULE_SHADERS})
    get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
    add_custom_command(TARGET copy-shaders POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHADER_FILE} ${SHADER_DST_DIR}/${SHADER_NAME}
    )
endforeach()
