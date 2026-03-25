# yetty sources and subdirectories

#-----------------------------------------------------------------------------
# Feature subdirectories — guarded by YETTY_ENABLE_FEATURE_*
#-----------------------------------------------------------------------------

if(YETTY_ENABLE_FEATURE_YGUI_C)
    add_subdirectory(ygui-c)
endif()

if(YETTY_ENABLE_FEATURE_YDRAW_ZOO)
    add_subdirectory(ydraw-zoo)
endif()

if(YETTY_ENABLE_FEATURE_YDRAW_MAZE)
    add_subdirectory(ydraw-maze)
endif()

if(YETTY_ENABLE_FEATURE_YGRID)
    add_subdirectory(cards/ygrid)
endif()

if(YETTY_ENABLE_FEATURE_MSDF_GEN)
    add_subdirectory(msdf-gen)
endif()

if(YETTY_ENABLE_FEATURE_RPC)
    add_subdirectory(rpc)
endif()

# Core sources - ALL sources needed for yetty executable (platform-agnostic)
set(YETTY_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/yetty.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/incbin-assets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/osc-scanner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terminal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terminal-view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gpu-screen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/osc-command.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gpu-allocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gpu-memory-manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gpu-buffer-manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gpu-texture-manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gpu-screen-manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/card-factory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/card.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/name-generator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ygui/ygui-overlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/screen-draw-layer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/workspace.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/frame-renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/shader-manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/font-manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/msdf-atlas.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ms-msdf-font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vector-sdf-font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vector-coverage-font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/raster-font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bm-font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/shader-font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/shared-memory.cpp
    # Renderer sources (from subdirectories, empty if features disabled)
    ${YETTY_ZOO_SOURCES}
    ${YETTY_MAZE_SOURCES}
    ${YETTY_YGRID_SOURCES}
    PARENT_SCOPE
)

# Desktop-only sources (Linux, macOS - NOT Windows)
set(YETTY_DESKTOP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/msdf-cdb-provider.cpp
    ${YETTY_MSDF_GEN_SOURCES}
    ${YETTY_RPC_SOURCES}
    PARENT_SCOPE
)

# Windows-specific sources
set(YETTY_WINDOWS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/msdf-cdb-provider.cpp
    ${YETTY_MSDF_GEN_SOURCES}
    ${YETTY_RPC_SOURCES}
    PARENT_SCOPE
)

# WebAssembly-specific sources
set(YETTY_WEB_SOURCES
    PARENT_SCOPE
)

# Android-specific sources
set(YETTY_ANDROID_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/msdf-cdb-provider.cpp
    PARENT_SCOPE
)

# iPhone/iOS-specific sources
set(YETTY_IOS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/msdf-cdb-provider.cpp
    PARENT_SCOPE
)

# Include directories for renderer sources
set(YETTY_RENDERER_INCLUDES
    ${YETTY_ZOO_INCLUDES}
    ${YETTY_MAZE_INCLUDES}
    PARENT_SCOPE
)

#-----------------------------------------------------------------------------
# Library subdirectories — guarded by YETTY_ENABLE_FEATURE_*
#-----------------------------------------------------------------------------

if(YETTY_ENABLE_FEATURE_BASE)
    add_subdirectory(base)
endif()

if(YETTY_ENABLE_FEATURE_FONT)
    add_subdirectory(font)
endif()

if(YETTY_ENABLE_FEATURE_YECHO)
    add_subdirectory(yecho)
endif()

if(YETTY_ENABLE_FEATURE_YCAT)
    if(NOT EMSCRIPTEN AND NOT YETTY_IOS)
        add_subdirectory(ycat)
    endif()
endif()

if(YETTY_ENABLE_FEATURE_YDRAW)
    add_subdirectory(ydraw)
endif()

if(YETTY_ENABLE_FEATURE_YRICH)
    add_subdirectory(yrich)
endif()

if(YETTY_ENABLE_FEATURE_YPAINT)
    add_subdirectory(ypaint)
endif()

if(YETTY_ENABLE_FEATURE_DIAGRAM)
    add_subdirectory(diagram)
endif()

if(YETTY_ENABLE_FEATURE_YGRID)
    add_subdirectory(ygrid)
endif()

if(YETTY_ENABLE_FEATURE_YTHORVG)
    if(YETTY_ENABLE_LIB_THORVG)
        add_subdirectory(ythorvg)
    endif()
endif()

if(YETTY_ENABLE_FEATURE_YVIDEO)
    add_subdirectory(yvideo)
endif()

if(YETTY_ENABLE_FEATURE_CARDS)
    add_subdirectory(cards)
endif()

if(YETTY_ENABLE_FEATURE_YAST)
    add_subdirectory(yast)
endif()

if(YETTY_ENABLE_FEATURE_YFSVM)
    add_subdirectory(yfsvm)
endif()

if(YETTY_ENABLE_FEATURE_YPLOT)
    add_subdirectory(yplot)
endif()

if(YETTY_ENABLE_FEATURE_MSDF_WGSL)
    add_subdirectory(msdf-wgsl)
endif()

if(YETTY_ENABLE_FEATURE_SHADERS)
    add_subdirectory(shaders)
endif()

if(YETTY_ENABLE_FEATURE_VNC)
    if(NOT YETTY_ANDROID)
        add_subdirectory(vnc)
    endif()
endif()

if(YETTY_ENABLE_FEATURE_TELNET)
    add_subdirectory(telnet)
endif()

if(YETTY_ENABLE_FEATURE_SSH)
    add_subdirectory(ssh)
endif()
