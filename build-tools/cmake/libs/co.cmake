# libco - minimal stackful coroutines (ISC license)
# https://github.com/higan-emu/libco
#
# Provides target `co` exposing libco.h and the platform-appropriate
# stack-switch implementation. On amd64/aarch64/arm/x86/ppc the backend is
# tiny inline assembly; on unsupported archs libco falls back to ucontext.
#
# Not built for emscripten — the webasm coroutine backend uses
# emscripten_fiber_t (Asyncify) instead, see ycoroutine.c on that platform.

if(TARGET co)
    return()
endif()

if(EMSCRIPTEN)
    return()
endif()

CPMAddPackage(
    NAME libco
    GITHUB_REPOSITORY higan-emu/libco
    GIT_TAG master
    DOWNLOAD_ONLY YES
)

if(libco_ADDED)
    add_library(co STATIC ${libco_SOURCE_DIR}/libco.c)
    target_include_directories(co PUBLIC ${libco_SOURCE_DIR})
    set_target_properties(co PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_STANDARD 99
    )
    message(STATUS "libco: Built from source (higan-emu/libco master)")
endif()
