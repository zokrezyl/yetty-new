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

# Yetty sources (src/yetty/*.c)
set(YETTY_SOURCES
    ${YETTY_ROOT}/src/yetty/yetty.c
    ${YETTY_ROOT}/src/yetty/config.c
    ${YETTY_ROOT}/src/yetty/ytrace.c
)


# Render subsystem
add_subdirectory(yrender)

# WebGPU utilities
add_subdirectory(ywebgpu)

# Core sources (src/yetty/core/*.cpp)
set(YETTY_CORE_SOURCES "")

# Term subsystem
add_subdirectory(yterm)

set(YETTY_SOURCES ${YETTY_SOURCES} PARENT_SCOPE)
set(YETTY_CORE_SOURCES ${YETTY_CORE_SOURCES} PARENT_SCOPE)

# Desktop-only sources (Linux, macOS - NOT Windows)
set(YETTY_DESKTOP_SOURCES "" PARENT_SCOPE)

# Windows-specific sources
set(YETTY_WINDOWS_SOURCES "" PARENT_SCOPE)

# WebAssembly-specific sources
set(YETTY_WEB_SOURCES "" PARENT_SCOPE)

# Android-specific sources
set(YETTY_ANDROID_SOURCES "" PARENT_SCOPE)

# iPhone/iOS-specific sources
set(YETTY_IOS_SOURCES "" PARENT_SCOPE)

# Include directories for renderer sources
set(YETTY_RENDERER_INCLUDES "" PARENT_SCOPE)

#-----------------------------------------------------------------------------
# Library subdirectories — guarded by YETTY_ENABLE_FEATURE_*
#-----------------------------------------------------------------------------

if(YETTY_ENABLE_FEATURE_YCORE)
    add_subdirectory(ycore)
endif()

if(YETTY_ENABLE_FEATURE_YFONT)
    add_subdirectory(yfont)
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
    add_subdirectory(ypaint-core)
    add_subdirectory(ysdf)
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

if(YETTY_ENABLE_FEATURE_YSHADERS)
    add_subdirectory(yshaders)
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
