#=============================================================================
# Yetty Build Variables
#
# Central place to enable/disable all libraries and features.
# Include this BEFORE any library or feature includes.
#
# YETTY_ENABLE_LIB_*     — third-party library dependencies
# YETTY_ENABLE_FEATURE_* — internal source directories / modules
#=============================================================================

#-----------------------------------------------------------------------------
# Libraries (third-party dependencies)
#-----------------------------------------------------------------------------

# Core / always needed
option(YETTY_ENABLE_LIB_INCBIN      "incbin — binary embedding"             ON)
option(YETTY_ENABLE_LIB_ARGS        "args — command line parser"             ON)
option(YETTY_ENABLE_LIB_LZ4         "lz4 — compression"                     OFF)
option(YETTY_ENABLE_LIB_LIBUV       "libuv — event loop"                    ON)
option(YETTY_ENABLE_LIB_GLM         "glm — math"                            OFF)
option(YETTY_ENABLE_LIB_STB         "stb — image loading"                   OFF)
option(YETTY_ENABLE_LIB_YAML_CPP    "yaml-cpp — config parsing (C++)"       OFF)
option(YETTY_ENABLE_LIB_LIBYAML     "libyaml — config parsing (C)"          ON)
option(YETTY_ENABLE_LIB_SPDLOG      "spdlog — logging backend"              ON)
option(YETTY_ENABLE_LIB_YTRACE      "ytrace — tracing framework"            ON)
option(YETTY_ENABLE_LIB_MSGPACK     "msgpack — serialization"               ON)
option(YETTY_ENABLE_LIB_WEBGPU      "webgpu/dawn — GPU backend"             ON)
option(YETTY_ENABLE_LIB_VTERM       "libvterm — terminal emulation"          ON)
option(YETTY_ENABLE_LIB_ZLIB        "zlib — compression"                    ON)
option(YETTY_ENABLE_LIB_LIBPNG      "libpng — PNG support"                  ON)
option(YETTY_ENABLE_LIB_FREETYPE    "freetype — font rendering"             ON)
option(YETTY_ENABLE_LIB_MSDFGEN     "msdfgen — MSDF font generation"        OFF)
option(YETTY_ENABLE_LIB_CDB         "cdb — constant database"               OFF)

# Media / codecs
option(YETTY_ENABLE_LIB_LIBJPEG_TURBO "libjpeg-turbo — JPEG support"       ON)
option(YETTY_ENABLE_LIB_DAV1D       "dav1d — AV1 decoder"                   OFF)
option(YETTY_ENABLE_LIB_OPENH264    "openh264 — H.264 codec"                OFF)
option(YETTY_ENABLE_LIB_MINIMP4     "minimp4 — MP4 container"               OFF)

# Misc
option(YETTY_ENABLE_LIB_TREESITTER  "tree-sitter — parsing"                 OFF)
option(YETTY_ENABLE_LIB_WASM3       "wasm3 — WASM interpreter"              OFF)
option(YETTY_ENABLE_LIB_LIBSSH2     "libssh2 — SSH protocol"                OFF)
option(YETTY_ENABLE_LIB_THORVG      "thorvg — SVG/Lottie rendering"         OFF)

# Platform-conditional (desktop only: linux, macos, windows)
option(YETTY_ENABLE_LIB_GLFW        "glfw — windowing (desktop only)"       ON)
option(YETTY_ENABLE_LIB_LIBMAGIC    "libmagic — file type detection"         OFF)

# Virtual machine (--virtual flag: run shell in RISC-V Linux VM)
option(YETTY_ENABLE_LIB_TINYEMU     "tinyemu — RISC-V emulator for --virtual" ON)
option(YETTY_ENABLE_LIB_QEMU        "qemu — QEMU RISC-V emulator (via telnet)" ON)

#-----------------------------------------------------------------------------
# Features (internal source directories / modules)
# All OFF by default — enable as they are ported to the new architecture
#-----------------------------------------------------------------------------

# Core modules
option(YETTY_ENABLE_FEATURE_YCORE      "yetty_core — core utilities"          ON)
option(YETTY_ENABLE_FEATURE_YFONT      "yetty_yfont — font subsystem"          ON)
option(YETTY_ENABLE_FEATURE_YSHADERS   "shaders — WGSL shader sources"        ON)
option(YETTY_ENABLE_FEATURE_MSDF_WGSL "ymsdf-wgsl — MSDF shader lib"         OFF)
option(YETTY_ENABLE_FEATURE_MSDF_GEN  "ymsdf-gen — MSDF font generator"       OFF)

# Terminal / display
option(YETTY_ENABLE_FEATURE_YECHO     "yecho — echo/display"                 OFF)
option(YETTY_ENABLE_FEATURE_YGUI_C    "ygui-c — C GUI bindings"              OFF)

# Drawing / rendering
option(YETTY_ENABLE_FEATURE_YDRAW     "ydraw — 2D vector drawing"            OFF)
option(YETTY_ENABLE_FEATURE_YDRAW_ZOO "ydraw-zoo — ydraw demo shapes"        OFF)
option(YETTY_ENABLE_FEATURE_YDRAW_MAZE "ydraw-maze — ydraw maze demo"        OFF)
option(YETTY_ENABLE_FEATURE_YPAINT    "ypaint — painting"                    ON)
option(YETTY_ENABLE_FEATURE_YRICH     "yrich — rich text"                    OFF)
option(YETTY_ENABLE_FEATURE_DIAGRAM   "diagram — diagram rendering"          OFF)
option(YETTY_ENABLE_FEATURE_YPLOT     "yplot — plotting"                     ON)

# Cards
option(YETTY_ENABLE_FEATURE_CARDS     "cards — card plugin system"           OFF)
option(YETTY_ENABLE_FEATURE_YGRID     "ygrid — grid card"                    OFF)
option(YETTY_ENABLE_FEATURE_YTHORVG   "ythorvg — thorvg card"               OFF)

# Video / media
option(YETTY_ENABLE_FEATURE_YVIDEO    "yvideo — video codec support"         OFF)

# Network / connectivity
option(YETTY_ENABLE_FEATURE_YVNC       "vnc — VNC client/server"              ON)
option(YETTY_ENABLE_FEATURE_TELNET    "telnet — telnet connectivity"         ON)
option(YETTY_ENABLE_FEATURE_SSH       "ssh — SSH connectivity"               OFF)
option(YETTY_ENABLE_FEATURE_YRPC      "yrpc — msgpack-RPC interface"         ON)

# Misc
option(YETTY_ENABLE_FEATURE_YCDB      "ycdb — constant database wrapper"     ON)
option(YETTY_ENABLE_FEATURE_YEXPR     "yexpr — expression parser"            ON)
option(YETTY_ENABLE_FEATURE_YFSVM     "yfsvm — fragment shader VM"           ON)
option(YETTY_ENABLE_FEATURE_YIMAGE    "yimage — image complex primitive"     ON)
option(YETTY_ENABLE_FEATURE_YMSDF_GEN "ymsdf-gen — MSDF glyph generator"    ON)
option(YETTY_ENABLE_FEATURE_YCAT      "ycat — file viewer"                   OFF)

# Build pipeline
option(YETTY_ENABLE_FEATURE_ASSETS    "assets — runtime asset copying"       ON)
option(YETTY_ENABLE_FEATURE_CDB_GEN   "cdb-gen — CDB font generation"       ON)
option(YETTY_ENABLE_FEATURE_TESTS     "tests — unit tests"                   OFF)
option(YETTY_ENABLE_FEATURE_DEMO      "demo — demo programs"                 OFF)

# Tools (each tool has its own option)
option(YETTY_ENABLE_TOOL_GPU_INFO        "gpu-info tool"                     OFF)
option(YETTY_ENABLE_TOOL_CARD_RUNNER     "card-runner tool"                  OFF)
option(YETTY_ENABLE_TOOL_YDRAW_GENERATOR "ydraw-generator tool"              OFF)
option(YETTY_ENABLE_TOOL_YPAINT_BENCH    "ypaint-bench tool"                 OFF)
option(YETTY_ENABLE_TOOL_YDRAW_MAZE      "ydraw-maze tool"                   OFF)
option(YETTY_ENABLE_TOOL_YDRAW_ZOO       "ydraw-zoo tool"                    OFF)
option(YETTY_ENABLE_TOOL_YMUX            "ymux tool"                         OFF)
option(YETTY_ENABLE_TOOL_YFLAME          "yflame tool"                       OFF)
option(YETTY_ENABLE_TOOL_PDF2YDRAW       "pdf2ydraw tool"                    OFF)
option(YETTY_ENABLE_TOOL_HTML2YDRAW      "html2ydraw tool"                   OFF)
option(YETTY_ENABLE_TOOL_YHTML_MACHINE   "yhtml-machine tool"                OFF)
option(YETTY_ENABLE_TOOL_YBROWSER        "ybrowser tool"                     OFF)
option(YETTY_ENABLE_TOOL_VNC_RECORDER    "vnc-recorder tool"                 OFF)
option(YETTY_ENABLE_TOOL_YDOC            "ydoc tool"                         OFF)
option(YETTY_ENABLE_TOOL_YSPREADSHEET    "yspreadsheet tool"                 OFF)
option(YETTY_ENABLE_TOOL_YSLIDES         "yslides tool"                      OFF)
option(YETTY_ENABLE_TOOL_QA              "qa static analysis tools"          ON)

# Auto-disable QA tools for cross-compilation (requires host LLVM/Clang libs)
# Also disabled on macOS for now — tools/qa/CMakeLists.txt hardcodes Linux LLVM paths.
if(YETTY_ENABLE_TOOL_QA)
    if(YETTY_ANDROID OR YETTY_IOS OR EMSCRIPTEN OR CMAKE_CROSSCOMPILING OR APPLE)
        message(STATUS "Disabling QA tools (cross-compilation or macOS)")
        set(YETTY_ENABLE_TOOL_QA OFF CACHE BOOL "" FORCE)
    endif()
endif()

option(YETTY_ENABLE_FEATURE_JSLINUX   "jslinux — JSLinux integration"        ON)

# Desktop-only features
option(YETTY_ENABLE_FEATURE_GPU       "gpu — GPU tools (desktop)"            OFF)
option(YETTY_ENABLE_FEATURE_CLIENT    "client — client tools (desktop)"      OFF)
option(YETTY_ENABLE_FEATURE_YTOP      "ytop — system monitor (desktop)"      OFF)
