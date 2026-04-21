# Shared configuration for all platforms
# Include this before platform-specific target files

# Note: variables.cmake is included from root CMakeLists.txt

# Determine platform name for config file selection
if(YETTY_ANDROID)
    set(YETTY_PLATFORM "android")
elseif(YETTY_IOS)
    set(YETTY_PLATFORM "ios")
elseif(EMSCRIPTEN)
    set(YETTY_PLATFORM "webasm")
elseif(WIN32)
    set(YETTY_PLATFORM "windows")
elseif(APPLE)
    set(YETTY_PLATFORM "macos")
else()
    set(YETTY_PLATFORM "linux")
endif()

# Platform abstraction sources (thread, term, fs)
if(WIN32)
    set(YETTY_PLATFORM_THREAD_SOURCES
        ${YETTY_ROOT}/src/yetty/yplatform/windows/thread.c
        ${YETTY_ROOT}/src/yetty/yplatform/windows/term.c
        ${YETTY_ROOT}/src/yetty/yplatform/windows/fs.c
    )
else()
    set(YETTY_PLATFORM_THREAD_SOURCES
        ${YETTY_ROOT}/src/yetty/yplatform/shared/thread.c
        ${YETTY_ROOT}/src/yetty/yplatform/shared/term.c
        ${YETTY_ROOT}/src/yetty/yplatform/shared/fs.c
    )
endif()

# Auto-generate MSDF CDB fonts if not present (must run before incbin)
if(YETTY_ENABLE_FEATURE_MSDF_GEN)
    include(${YETTY_ROOT}/build-tools/cmake/prepare-assets.cmake)
endif()

#-----------------------------------------------------------------------------
# Libraries — guarded by YETTY_ENABLE_LIB_*
#-----------------------------------------------------------------------------

if(YETTY_ENABLE_LIB_INCBIN)
    include(${YETTY_ROOT}/build-tools/cmake/incbin.cmake)
endif()

if(YETTY_ENABLE_LIB_ARGS)
    include(${YETTY_ROOT}/build-tools/cmake/libs/args.cmake)
endif()

if(YETTY_ENABLE_LIB_LZ4)
    include(${YETTY_ROOT}/build-tools/cmake/libs/lz4.cmake)
endif()

if(YETTY_ENABLE_LIB_LIBUV)
    include(${YETTY_ROOT}/build-tools/cmake/libs/libuv.cmake)
endif()

if(YETTY_ENABLE_LIB_GLM)
    include(${YETTY_ROOT}/build-tools/cmake/libs/glm.cmake)
endif()

if(YETTY_ENABLE_LIB_STB)
    include(${YETTY_ROOT}/build-tools/cmake/libs/stb.cmake)
endif()

if(YETTY_ENABLE_LIB_YAML_CPP)
    include(${YETTY_ROOT}/build-tools/cmake/libs/yaml-cpp.cmake)
endif()

if(YETTY_ENABLE_LIB_LIBYAML)
    include(${YETTY_ROOT}/build-tools/cmake/libs/libyaml.cmake)
endif()

if(YETTY_ENABLE_LIB_SPDLOG)
    include(${YETTY_ROOT}/build-tools/cmake/libs/spdlog.cmake)
endif()

if(YETTY_ENABLE_LIB_YTRACE)
    include(${YETTY_ROOT}/build-tools/cmake/libs/ytrace.cmake)
endif()

if(YETTY_ENABLE_LIB_MSGPACK)
    include(${YETTY_ROOT}/build-tools/cmake/libs/msgpack.cmake)
endif()

if(YETTY_ENABLE_LIB_WEBGPU)
    include(${YETTY_ROOT}/build-tools/cmake/libs/webgpu.cmake)
endif()

if(YETTY_ENABLE_LIB_VTERM)
    include(${YETTY_ROOT}/build-tools/cmake/libs/vterm.cmake)
endif()

if(YETTY_ENABLE_LIB_ZLIB)
    include(${YETTY_ROOT}/build-tools/cmake/libs/zlib.cmake)
endif()

if(YETTY_ENABLE_LIB_LIBPNG)
    include(${YETTY_ROOT}/build-tools/cmake/libs/libpng.cmake)
endif()

if(YETTY_ENABLE_LIB_FREETYPE)
    include(${YETTY_ROOT}/build-tools/cmake/FreeType.cmake)
endif()

if(YETTY_ENABLE_LIB_MSDFGEN)
    include(${YETTY_ROOT}/build-tools/cmake/libs/msdfgen.cmake)
endif()

if(YETTY_ENABLE_LIB_CDB)
    include(${YETTY_ROOT}/build-tools/cmake/libs/cdb.cmake)
endif()

if(YETTY_ENABLE_LIB_THORVG)
    include(${YETTY_ROOT}/build-tools/cmake/thorvg.cmake)
endif()

if(YETTY_ENABLE_LIB_TREESITTER)
    include(${YETTY_ROOT}/build-tools/cmake/TreeSitter.cmake)
endif()

if(YETTY_ENABLE_LIB_DAV1D)
    include(${YETTY_ROOT}/build-tools/cmake/Dav1d.cmake)
endif()

if(YETTY_ENABLE_LIB_OPENH264)
    include(${YETTY_ROOT}/build-tools/cmake/openh264.cmake)
endif()

if(YETTY_ENABLE_LIB_MINIMP4)
    include(${YETTY_ROOT}/build-tools/cmake/minimp4.cmake)
endif()

if(YETTY_ENABLE_LIB_WASM3)
    include(${YETTY_ROOT}/build-tools/cmake/libs/wasm3.cmake)
endif()

if(YETTY_ENABLE_LIB_LIBSSH2)
    include(${YETTY_ROOT}/build-tools/cmake/libs/libssh2.cmake)
endif()

if(YETTY_ENABLE_LIB_LIBJPEG_TURBO)
    include(${YETTY_ROOT}/build-tools/cmake/libs/libjpeg-turbo.cmake)
endif()

# Add src/yetty (populates YETTY_SOURCES, YETTY_CORE_SOURCES, builds feature libraries)
add_subdirectory(${YETTY_ROOT}/src/yetty ${CMAKE_BINARY_DIR}/src/yetty)

# Common include directories
set(YETTY_INCLUDES
    ${YETTY_ROOT}/src
    ${YETTY_ROOT}/include
)

# Common compile definitions
set(YETTY_DEFINITIONS
    CMAKE_SOURCE_DIR="${YETTY_ROOT}"
)

if(YETTY_ENABLE_LIB_THORVG)
    list(APPEND YETTY_DEFINITIONS YETTY_HAS_THORVG=1)
endif()

# Common libraries to link (only include what's enabled)
set(YETTY_LIBS "")

# Third-party library link targets
if(YETTY_ENABLE_LIB_WEBGPU)
    list(APPEND YETTY_LIBS webgpu)
endif()
if(YETTY_ENABLE_LIB_GLM)
    list(APPEND YETTY_LIBS glm::glm)
endif()
if(YETTY_ENABLE_LIB_STB)
    list(APPEND YETTY_LIBS stb)
endif()
if(YETTY_ENABLE_LIB_YAML_CPP)
    list(APPEND YETTY_LIBS yaml-cpp)
endif()
if(YETTY_ENABLE_LIB_LIBYAML)
    list(APPEND YETTY_LIBS yaml)
endif()
if(YETTY_ENABLE_LIB_VTERM)
    list(APPEND YETTY_LIBS vterm)
endif()
if(YETTY_ENABLE_LIB_MSGPACK)
    list(APPEND YETTY_LIBS msgpack-cxx)
endif()
if(YETTY_ENABLE_LIB_MSDFGEN)
    list(APPEND YETTY_LIBS msdfgen::msdfgen-core msdfgen::msdfgen-ext ${FREETYPE_ALL_LIBS} ${BROTLIDEC_LIBRARIES})
endif()
if(YETTY_ENABLE_LIB_CDB)
    list(APPEND YETTY_LIBS cdb-wrapper)
endif()
if(YETTY_ENABLE_LIB_ARGS)
    list(APPEND YETTY_LIBS args)
endif()
if(YETTY_ENABLE_LIB_LZ4)
    list(APPEND YETTY_LIBS lz4_static)
endif()
if(YETTY_ENABLE_LIB_LIBUV)
    list(APPEND YETTY_LIBS uv_a)
endif()
if(YETTY_ENABLE_LIB_YTRACE)
    list(APPEND YETTY_LIBS ytrace::ytrace)
endif()
if(YETTY_ENABLE_LIB_GLFW)
    list(APPEND YETTY_LIBS glfw glfw3webgpu)
endif()
if(YETTY_ENABLE_LIB_LIBJPEG_TURBO)
    list(APPEND YETTY_LIBS turbojpeg-static)
endif()
if(YETTY_ENABLE_LIB_ZLIB)
    list(APPEND YETTY_LIBS zlibstatic)
endif()

# Core libraries (always linked)
# Note: yetty_yui comes first because it depends on yetty_term
list(APPEND YETTY_LIBS yetty_yui yetty_term yetty_yrender yetty_webgpu)

# Feature library link targets
if(YETTY_ENABLE_FEATURE_BASE)
    list(APPEND YETTY_LIBS yetty_base)
endif()
if(YETTY_ENABLE_FEATURE_FONT)
    list(APPEND YETTY_LIBS yetty_font)
endif()
if(YETTY_ENABLE_FEATURE_YECHO)
    list(APPEND YETTY_LIBS yetty_yecho)
endif()
if(YETTY_ENABLE_FEATURE_YDRAW)
    list(APPEND YETTY_LIBS yetty_ydraw)
endif()
if(YETTY_ENABLE_FEATURE_YPAINT)
    list(APPEND YETTY_LIBS yetty_ypaint)
endif()
if(YETTY_ENABLE_FEATURE_DIAGRAM)
    list(APPEND YETTY_LIBS yetty_diagram)
endif()
if(YETTY_ENABLE_FEATURE_YGRID)
    list(APPEND YETTY_LIBS ygrid)
endif()
if(YETTY_ENABLE_FEATURE_CARDS)
    list(APPEND YETTY_LIBS yetty_cards)
endif()
if(YETTY_ENABLE_FEATURE_YAST)
    list(APPEND YETTY_LIBS yetty_yast)
endif()
if(YETTY_ENABLE_FEATURE_TELNET)
    list(APPEND YETTY_LIBS yetty_telnet)
endif()
if(YETTY_ENABLE_FEATURE_SSH)
    list(APPEND YETTY_LIBS yetty_ssh)
endif()
if(YETTY_ENABLE_FEATURE_MSDF_WGSL)
    list(APPEND YETTY_LIBS msdf-wgsl)
endif()
if(YETTY_ENABLE_FEATURE_GPU)
    list(APPEND YETTY_LIBS yetty_gpu)
endif()
if(YETTY_ENABLE_FEATURE_YVNC)
    list(APPEND YETTY_LIBS yetty_vnc)
endif()
if(YETTY_ENABLE_FEATURE_YRPC)
    list(APPEND YETTY_LIBS yetty_yrpc)
endif()
if(YETTY_ENABLE_FEATURE_YVIDEO)
    list(APPEND YETTY_LIBS yetty_yvideo)
endif()

#-----------------------------------------------------------------------------
# Prepare assets at configure time (generates CDB files if missing)
#-----------------------------------------------------------------------------
if(YETTY_ENABLE_FEATURE_CDB_GEN)
    include(${YETTY_ROOT}/build-tools/cmake/prepare-assets.cmake)
endif()

#-----------------------------------------------------------------------------
# yetty_embed_assets(TARGET)
#
# Embeds shaders, fonts, and CDB files into the target binary.
# Call this AFTER creating the target with add_executable/add_library.
# WebAssembly: provides empty stubs (uses --preload-file instead)
#-----------------------------------------------------------------------------
function(yetty_embed_assets TARGET)
    if(NOT YETTY_ENABLE_LIB_INCBIN)
        return()
    endif()

    # Collect ALL data assets into one build directory
    set(INCBIN_DATA_DIR "${CMAKE_BINARY_DIR}/incbin-data")
    file(MAKE_DIRECTORY "${INCBIN_DATA_DIR}")
    file(MAKE_DIRECTORY "${INCBIN_DATA_DIR}/shaders")
    file(MAKE_DIRECTORY "${INCBIN_DATA_DIR}/fonts")
    file(MAKE_DIRECTORY "${INCBIN_DATA_DIR}/msdf-fonts")

    # Copy logo
    file(COPY "${YETTY_ROOT}/docs/logo.jpeg" DESTINATION "${INCBIN_DATA_DIR}")

    # Collect shaders from module locations
    file(COPY "${YETTY_ROOT}/src/yetty/yterm/text-layer.wgsl" DESTINATION "${INCBIN_DATA_DIR}/shaders")
    file(COPY "${YETTY_ROOT}/src/yetty/yterm/ypaint-layer.wgsl" DESTINATION "${INCBIN_DATA_DIR}/shaders")
    file(COPY "${YETTY_ROOT}/src/yetty/yrender/blend.wgsl" DESTINATION "${INCBIN_DATA_DIR}/shaders")
    file(RENAME "${INCBIN_DATA_DIR}/shaders/blend.wgsl" "${INCBIN_DATA_DIR}/shaders/blender.wgsl")
    file(COPY "${YETTY_ROOT}/src/yetty/yfont/ms-msdf-font.wgsl" DESTINATION "${INCBIN_DATA_DIR}/shaders")
    file(COPY "${YETTY_ROOT}/src/yetty/yfont/msdf-font.wgsl" DESTINATION "${INCBIN_DATA_DIR}/shaders")
    file(COPY "${YETTY_ROOT}/src/yetty/yfont/raster-font.wgsl" DESTINATION "${INCBIN_DATA_DIR}/shaders")

    # Copy fonts
    file(GLOB FONT_FILES "${YETTY_ROOT}/assets/fonts/*.ttf")
    foreach(FONT_FILE ${FONT_FILES})
        file(COPY "${FONT_FILE}" DESTINATION "${INCBIN_DATA_DIR}/fonts")
    endforeach()

    # Copy msdf-fonts (generated at configure time)
    file(GLOB MSDF_FILES "${CMAKE_BINARY_DIR}/assets/msdf-fonts/*.cdb")
    foreach(MSDF_FILE ${MSDF_FILES})
        file(COPY "${MSDF_FILE}" DESTINATION "${INCBIN_DATA_DIR}/msdf-fonts")
    endforeach()

    # Embed ALL data assets (brotli compressed)
    incbin_add_directory(${TARGET} "data" "${INCBIN_DATA_DIR}" "*" TRUE)

    # Collect config into separate build directory
    set(INCBIN_CONFIG_DIR "${CMAKE_BINARY_DIR}/incbin-config")
    file(MAKE_DIRECTORY "${INCBIN_CONFIG_DIR}")
    set(DEFAULT_CONFIG_FILE "${YETTY_ROOT}/assets/default-config-${YETTY_PLATFORM}.yaml")
    if(NOT EXISTS "${DEFAULT_CONFIG_FILE}")
        set(DEFAULT_CONFIG_FILE "${YETTY_ROOT}/assets/default-config.yaml")
    endif()
    file(COPY "${DEFAULT_CONFIG_FILE}" DESTINATION "${INCBIN_CONFIG_DIR}")
    get_filename_component(CONFIG_FILENAME "${DEFAULT_CONFIG_FILE}" NAME)
    # Rename to config.yaml when extracted
    file(RENAME "${INCBIN_CONFIG_DIR}/${CONFIG_FILENAME}" "${INCBIN_CONFIG_DIR}/config.yaml")

    # Embed config (not compressed)
    incbin_add_directory(${TARGET} "config" "${INCBIN_CONFIG_DIR}" "*" FALSE)

    # Embed TinyEMU runtime assets if enabled
    # Assets are built at configure time, then embedded via incbin_add_directory
    if(YETTY_ENABLE_LIB_TINYEMU)
        set(TINYEMU_ASSETS_DIR "${CMAKE_BINARY_DIR}/assets/tinyemu")
        set(INCBIN_TINYEMU_DIR "${CMAKE_BINARY_DIR}/incbin-tinyemu")

        # Create incbin staging directory and copy assets
        file(MAKE_DIRECTORY "${INCBIN_TINYEMU_DIR}")

        if(EXISTS "${TINYEMU_ASSETS_DIR}/kernel-riscv64.bin")
            file(COPY "${TINYEMU_ASSETS_DIR}/kernel-riscv64.bin" DESTINATION "${INCBIN_TINYEMU_DIR}")
        endif()
        if(EXISTS "${TINYEMU_ASSETS_DIR}/opensbi-fw_jump.elf")
            file(COPY "${TINYEMU_ASSETS_DIR}/opensbi-fw_jump.elf" DESTINATION "${INCBIN_TINYEMU_DIR}")
        endif()
        if(EXISTS "${TINYEMU_ASSETS_DIR}/root-riscv64.cfg")
            file(COPY "${TINYEMU_ASSETS_DIR}/root-riscv64.cfg" DESTINATION "${INCBIN_TINYEMU_DIR}")
        endif()

        # Create tarball of alpine-rootfs at configure time (preserves symlinks)
        if(EXISTS "${TINYEMU_ASSETS_DIR}/alpine-rootfs")
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E tar cf "${INCBIN_TINYEMU_DIR}/alpine-rootfs.tar" .
                WORKING_DIRECTORY "${TINYEMU_ASSETS_DIR}/alpine-rootfs"
            )
        endif()

        # Embed TinyEMU assets (not compressed - binary files)
        incbin_add_directory(${TARGET} "tinyemu" "${INCBIN_TINYEMU_DIR}" "*" FALSE)
    endif()

    # Make manifest headers available
    target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endfunction()
