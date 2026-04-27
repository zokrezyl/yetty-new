# glfw + glfw3webgpu — desktop windowing.
#
# Consumes prebuilt static libs + headers from the 3rdparty releases
# published by build-3rdparty-glfw.yml + build-3rdparty-glfw3webgpu.yml.
# The from-source builds live in build-tools/3rdparty/{glfw,glfw3webgpu}/.

include_guard(GLOBAL)
include(${YETTY_ROOT}/build-tools/cmake/3rdparty-fetch.cmake)

if(TARGET glfw)
    return()
endif()

if(WIN32)
    message(FATAL_ERROR
        "glfw: no windows-x86_64 tarball is published yet — yetty.exe is \
being switched to native MSVC and the glfw MSVC build path will land \
together with that work (see the windows-libs-msvc branch).")
endif()

yetty_3rdparty_fetch(glfw _GLFW_DIR)

if(NOT EXISTS "${_GLFW_DIR}/lib/libglfw3.a")
    message(FATAL_ERROR "glfw: libglfw3.a not found in ${_GLFW_DIR}/lib/ — tarball layout changed?")
endif()

add_library(glfw STATIC IMPORTED GLOBAL)
set_target_properties(glfw PROPERTIES
    IMPORTED_LOCATION "${_GLFW_DIR}/lib/libglfw3.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_GLFW_DIR}/include"
)
# Platform link deps glfw needs from its consumers (mirrors what the
# upstream cmake config exports as INTERFACE_LINK_LIBRARIES).
if(APPLE)
    set_target_properties(glfw PROPERTIES
        INTERFACE_LINK_LIBRARIES "-framework Cocoa;-framework IOKit;-framework CoreFoundation;-framework QuartzCore"
    )
elseif(UNIX)
    set_target_properties(glfw PROPERTIES
        INTERFACE_LINK_LIBRARIES "pthread;dl;m;rt"
    )
endif()

message(STATUS "glfw: prebuilt v${YETTY_3RDPARTY_glfw_VERSION} (${_GLFW_DIR}/lib/libglfw3.a)")

#------------------------------------------------------------------------------
# glfw3webgpu — adapter that creates a WGPUSurface from a glfw window.
#------------------------------------------------------------------------------
if(NOT TARGET glfw3webgpu)
    yetty_3rdparty_fetch(glfw3webgpu _GLFW3WEBGPU_DIR)

    if(NOT EXISTS "${_GLFW3WEBGPU_DIR}/lib/libglfw3webgpu.a")
        message(FATAL_ERROR "glfw3webgpu: libglfw3webgpu.a not found in ${_GLFW3WEBGPU_DIR}/lib/ — tarball layout changed?")
    endif()

    add_library(glfw3webgpu STATIC IMPORTED GLOBAL)
    set_target_properties(glfw3webgpu PROPERTIES
        IMPORTED_LOCATION "${_GLFW3WEBGPU_DIR}/lib/libglfw3webgpu.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_GLFW3WEBGPU_DIR}/include"
        INTERFACE_LINK_LIBRARIES "glfw"
    )

    message(STATUS "glfw3webgpu: prebuilt @${YETTY_3RDPARTY_glfw3webgpu_VERSION} (${_GLFW3WEBGPU_DIR}/lib/libglfw3webgpu.a)")
endif()
