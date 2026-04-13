# Windows desktop build target

include(${YETTY_ROOT}/build-tools/cmake/targets/shared.cmake)

# Windows-specific libraries (guarded by variables.cmake)
if(YETTY_ENABLE_LIB_GLFW)
    include(${YETTY_ROOT}/build-tools/cmake/libs/glfw.cmake)
endif()

# Desktop-specific subdirectories
if(YETTY_ENABLE_FEATURE_GPU)
    add_subdirectory(${YETTY_ROOT}/src/yetty/gpu ${CMAKE_BINARY_DIR}/src/yetty/gpu)
endif()
if(YETTY_ENABLE_FEATURE_CLIENT)
    add_subdirectory(${YETTY_ROOT}/src/yetty/client ${CMAKE_BINARY_DIR}/src/yetty/client)
endif()

# Platform sources — Windows-specific + shared GLFW (C)
# Windows uses GLFW for window/surface but ConPTY for terminal and Windows pipes
set(YETTY_PLATFORM_SOURCES
    ${YETTY_ROOT}/src/yetty/yplatform/shared/glfw-main.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/glfw-surface.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/glfw-event-loop.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/glfw-window.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/glfw-clipboard-manager.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/libuv-event-loop.c
    ${YETTY_ROOT}/src/yetty/yplatform/windows/conpty.c
    ${YETTY_ROOT}/src/yetty/yplatform/windows/pipe.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/extract-assets.c
    ${YETTY_ROOT}/src/yetty/incbin-assets.c
    ${YETTY_ROOT}/src/yetty/yplatform/windows/platform-paths.c
)

# Create executable with core sources + platform
add_executable(yetty
    ${YETTY_CORE_SOURCES}
    ${YETTY_WINDOWS_SOURCES}
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
    YETTY_USE_DIRECTWRITE=1
    YETTY_USE_CONPTY=1
    YETTY_HAS_VNC=1
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

set_target_properties(yetty PROPERTIES ENABLE_EXPORTS TRUE)

target_link_libraries(yetty PRIVATE
    ${YETTY_LIBS}
    dwrite
    ws2_32
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

# Copy DirectX runtime DLLs needed by Dawn (shader compiler + DXIL signing)
if(WIN32)
    # Gather Windows SDK search dirs once, sorted descending (latest first)
    file(GLOB _sdk_bin_x64_dirs  "C:/Program Files (x86)/Windows Kits/10/bin/*/x64")
    file(GLOB _redist_x64_dirs   "C:/Program Files (x86)/Windows Kits/10/Redist/*/x64")
    if(_sdk_bin_x64_dirs)
        list(SORT _sdk_bin_x64_dirs ORDER DESCENDING)
    endif()
    if(_redist_x64_dirs)
        list(SORT _redist_x64_dirs ORDER DESCENDING)
    endif()

    # --- Find d3dcompiler_47.dll ---
    set(_d3dcompiler_dll "")
    if(EXISTS "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/d3dcompiler_47.dll")
        set(_d3dcompiler_dll "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/d3dcompiler_47.dll")
    else()
        foreach(_dir ${_redist_x64_dirs})
            if(EXISTS "${_dir}/d3dcompiler_47.dll")
                set(_d3dcompiler_dll "${_dir}/d3dcompiler_47.dll")
                break()
            endif()
        endforeach()
    endif()
    if(NOT _d3dcompiler_dll)
        foreach(_dir ${_sdk_bin_x64_dirs})
            if(EXISTS "${_dir}/d3dcompiler_47.dll")
                set(_d3dcompiler_dll "${_dir}/d3dcompiler_47.dll")
                break()
            endif()
        endforeach()
    endif()

    if(_d3dcompiler_dll)
        message(STATUS "Found d3dcompiler_47.dll: ${_d3dcompiler_dll}")
        add_custom_command(TARGET yetty POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_d3dcompiler_dll}"
                "$<TARGET_FILE_DIR:yetty>/d3dcompiler_47.dll"
            COMMENT "Copying d3dcompiler_47.dll"
        )
    else()
        message(WARNING "Could not find d3dcompiler_47.dll in Windows SDK")
    endif()

    # --- Find dxil.dll ---
    set(_dxil_dll "")
    foreach(_dir ${_sdk_bin_x64_dirs})
        if(EXISTS "${_dir}/dxil.dll")
            set(_dxil_dll "${_dir}/dxil.dll")
            break()
        endif()
    endforeach()
    if(NOT _dxil_dll)
        if(EXISTS "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/dxil.dll")
            set(_dxil_dll "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/dxil.dll")
        else()
            foreach(_dir ${_redist_x64_dirs})
                if(EXISTS "${_dir}/dxil.dll")
                    set(_dxil_dll "${_dir}/dxil.dll")
                    break()
                endif()
            endforeach()
        endif()
    endif()

    if(_dxil_dll)
        message(STATUS "Found dxil.dll: ${_dxil_dll}")
        add_custom_command(TARGET yetty POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dxil_dll}"
                "$<TARGET_FILE_DIR:yetty>/dxil.dll"
            COMMENT "Copying dxil.dll"
        )
    else()
        message(WARNING "Could not find dxil.dll in Windows SDK")
    endif()
endif()

# Verify all required assets are present
if(YETTY_ENABLE_FEATURE_ASSETS)
    add_custom_command(TARGET yetty POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DTARGET_TYPE=desktop -P ${YETTY_ROOT}/build-tools/cmake/verify-assets.cmake
        COMMENT "Verifying build assets..."
    )
endif()

# Tests
if(YETTY_ENABLE_FEATURE_TESTS)
    enable_testing()
    add_subdirectory(${YETTY_ROOT}/test/ut/windows ${CMAKE_BINARY_DIR}/test/ut/windows)
endif()
