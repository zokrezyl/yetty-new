# decode-ypaint — diagnostic CLI that walks a captured OSC stream from
# ycat (or any other ypaint-bin emitter) and re-runs the wire decode through
# yface + ypaint-core, printing per-envelope: OSC code, args, base64 size,
# decompressed size, magic check, and primitive count.
#
# Useful for isolating wire-format bugs from receiver-side rendering bugs:
# a clean run here means the bytes on the wire are valid end-to-end and any
# visible mis-render is in the canvas, the shader, or the GPU upload path.
#
# Pulls in the same shared sources ycat does (ytrace.c, yplatform/getopt.c,
# yplatform/shared/thread.c+term.c) so it can link the static yface +
# yterm-osc-args libs without depending on the full yetty executable build.

add_executable(decode-ypaint
    main.c
    # osc-args.c lives inside yetty_yterm, but linking yetty_yterm would
    # drag webgpu / vterm / yfont in too — way more than this tool needs.
    # Compile osc-args.c directly here, same trick ycat uses.
    ${YETTY_ROOT}/src/yetty/yterm/osc-args.c
    ${YETTY_ROOT}/src/yetty/ytrace.c
    ${YETTY_ROOT}/src/yetty/yplatform/getopt.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/thread.c
    ${YETTY_ROOT}/src/yetty/yplatform/shared/term.c
)

target_include_directories(decode-ypaint PRIVATE
    ${YETTY_ROOT}/include
    ${YETTY_ROOT}/src
)

find_package(Threads REQUIRED)
target_link_libraries(decode-ypaint PRIVATE
    yetty_yface
    yetty_ycore
    yetty_ypaint_core
    yetty_ypaint
    Threads::Threads
)

target_compile_options(decode-ypaint PRIVATE -Wall -Wextra)
