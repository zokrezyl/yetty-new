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
option(YETTY_ENABLE_LIB_INCBIN      "incbin — binary embedding"             OFF)
option(YETTY_ENABLE_LIB_ARGS        "args — command line parser"             OFF)
option(YETTY_ENABLE_LIB_LZ4         "lz4 — compression"                     OFF)
option(YETTY_ENABLE_LIB_LIBUV       "libuv — event loop"                    OFF)
option(YETTY_ENABLE_LIB_GLM         "glm — math"                            OFF)
option(YETTY_ENABLE_LIB_STB         "stb — image loading"                   OFF)
option(YETTY_ENABLE_LIB_YAML_CPP    "yaml-cpp — config parsing"             OFF)
option(YETTY_ENABLE_LIB_SPDLOG      "spdlog — logging backend"              ON)
option(YETTY_ENABLE_LIB_YTRACE      "ytrace — tracing framework"            ON)
option(YETTY_ENABLE_LIB_MSGPACK     "msgpack — serialization"               OFF)
option(YETTY_ENABLE_LIB_WEBGPU      "webgpu/dawn — GPU backend"             ON)
option(YETTY_ENABLE_LIB_VTERM       "libvterm — terminal emulation"          OFF)
option(YETTY_ENABLE_LIB_ZLIB        "zlib — compression"                    OFF)
option(YETTY_ENABLE_LIB_LIBPNG      "libpng — PNG support"                  OFF)
option(YETTY_ENABLE_LIB_MSDFGEN     "msdfgen — MSDF font generation"        OFF)
option(YETTY_ENABLE_LIB_CDB         "cdb — constant database"               OFF)

# Media / codecs
option(YETTY_ENABLE_LIB_LIBJPEG_TURBO "libjpeg-turbo — JPEG support"       OFF)
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

#-----------------------------------------------------------------------------
# Features (internal source directories / modules)
# All OFF by default — enable as they are ported to the new architecture
#-----------------------------------------------------------------------------

# Core modules
option(YETTY_ENABLE_FEATURE_BASE      "yetty_base — core utilities"          OFF)
option(YETTY_ENABLE_FEATURE_FONT      "yetty_font — font subsystem"          OFF)
option(YETTY_ENABLE_FEATURE_SHADERS   "shaders — WGSL shader sources"        OFF)
option(YETTY_ENABLE_FEATURE_MSDF_WGSL "msdf-wgsl — MSDF shader lib"         OFF)
option(YETTY_ENABLE_FEATURE_MSDF_GEN  "msdf-gen — MSDF font generator"       OFF)

# Terminal / display
option(YETTY_ENABLE_FEATURE_YECHO     "yecho — echo/display"                 OFF)
option(YETTY_ENABLE_FEATURE_YGUI_C    "ygui-c — C GUI bindings"              OFF)

# Drawing / rendering
option(YETTY_ENABLE_FEATURE_YDRAW     "ydraw — 2D vector drawing"            OFF)
option(YETTY_ENABLE_FEATURE_YDRAW_ZOO "ydraw-zoo — ydraw demo shapes"        OFF)
option(YETTY_ENABLE_FEATURE_YDRAW_MAZE "ydraw-maze — ydraw maze demo"        OFF)
option(YETTY_ENABLE_FEATURE_YPAINT    "ypaint — painting"                    OFF)
option(YETTY_ENABLE_FEATURE_YRICH     "yrich — rich text"                    OFF)
option(YETTY_ENABLE_FEATURE_DIAGRAM   "diagram — diagram rendering"          OFF)
option(YETTY_ENABLE_FEATURE_YPLOT     "yplot — plotting"                     OFF)

# Cards
option(YETTY_ENABLE_FEATURE_CARDS     "cards — card plugin system"           OFF)
option(YETTY_ENABLE_FEATURE_YGRID     "ygrid — grid card"                    OFF)
option(YETTY_ENABLE_FEATURE_YTHORVG   "ythorvg — thorvg card"               OFF)

# Video / media
option(YETTY_ENABLE_FEATURE_YVIDEO    "yvideo — video codec support"         OFF)

# Network / connectivity
option(YETTY_ENABLE_FEATURE_VNC       "vnc — VNC client/server"              OFF)
option(YETTY_ENABLE_FEATURE_TELNET    "telnet — telnet connectivity"         OFF)
option(YETTY_ENABLE_FEATURE_SSH       "ssh — SSH connectivity"               OFF)
option(YETTY_ENABLE_FEATURE_RPC       "rpc — msgpack-RPC interface"          OFF)

# Misc
option(YETTY_ENABLE_FEATURE_YAST      "yast — AST support"                   OFF)
option(YETTY_ENABLE_FEATURE_YFSVM     "yfsvm — filesystem VM"                OFF)
option(YETTY_ENABLE_FEATURE_YCAT      "ycat — file viewer"                   OFF)

# Build pipeline
option(YETTY_ENABLE_FEATURE_ASSETS    "assets — runtime asset copying"       OFF)
option(YETTY_ENABLE_FEATURE_CDB_GEN   "cdb-gen — CDB font generation"       OFF)
option(YETTY_ENABLE_FEATURE_TESTS     "tests — unit tests"                   OFF)
option(YETTY_ENABLE_FEATURE_TOOLS     "tools — build tools"                  OFF)
option(YETTY_ENABLE_FEATURE_DEMO      "demo — demo programs"                 OFF)
option(YETTY_ENABLE_FEATURE_JSLINUX   "jslinux — JSLinux integration"        OFF)

# Desktop-only features
option(YETTY_ENABLE_FEATURE_GPU       "gpu — GPU tools (desktop)"            OFF)
option(YETTY_ENABLE_FEATURE_CLIENT    "client — client tools (desktop)"      OFF)
option(YETTY_ENABLE_FEATURE_YTOP      "ytop — system monitor (desktop)"      OFF)
