# TinyEMU - RISC-V emulator (MIT license)
#
# Builds TinyEMU as a static library for embedding in yetty.
# Provides RISC-V emulation with virtio-console for PTY on platforms
# that don't support fork/exec (iOS).
#
if(TARGET tinyemu)
    return()
endif()

set(TINYEMU_DIR ${YETTY_ROOT}/src/tinyemu)

if(NOT EXISTS ${TINYEMU_DIR}/machine.c)
    message(FATAL_ERROR "TinyEMU source not found at ${TINYEMU_DIR}")
endif()

# Core emulator sources
set(TINYEMU_SOURCES
    ${TINYEMU_DIR}/virtio.c
    ${TINYEMU_DIR}/pci.c
    ${TINYEMU_DIR}/fs.c
    ${TINYEMU_DIR}/cutils.c
    ${TINYEMU_DIR}/iomem.c
    ${TINYEMU_DIR}/simplefb.c
    ${TINYEMU_DIR}/json.c
    ${TINYEMU_DIR}/machine.c
    ${TINYEMU_DIR}/riscv_machine.c
    ${TINYEMU_DIR}/softfp.c
    ${TINYEMU_DIR}/elf.c
    ${TINYEMU_DIR}/smp.c
)

# SLIRP user-space networking (always enabled)
set(TINYEMU_SLIRP_SOURCES
    ${TINYEMU_DIR}/slirp/bootp.c
    ${TINYEMU_DIR}/slirp/ip_icmp.c
    ${TINYEMU_DIR}/slirp/mbuf.c
    ${TINYEMU_DIR}/slirp/slirp.c
    ${TINYEMU_DIR}/slirp/tcp_output.c
    ${TINYEMU_DIR}/slirp/cksum.c
    ${TINYEMU_DIR}/slirp/ip_input.c
    ${TINYEMU_DIR}/slirp/misc.c
    ${TINYEMU_DIR}/slirp/socket.c
    ${TINYEMU_DIR}/slirp/tcp_subr.c
    ${TINYEMU_DIR}/slirp/udp.c
    ${TINYEMU_DIR}/slirp/if.c
    ${TINYEMU_DIR}/slirp/ip_output.c
    ${TINYEMU_DIR}/slirp/sbuf.c
    ${TINYEMU_DIR}/slirp/tcp_input.c
    ${TINYEMU_DIR}/slirp/tcp_timer.c
)

# Linux needs fs_disk.c for disk access
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND TINYEMU_SOURCES ${TINYEMU_DIR}/fs_disk.c)
endif()

# Get version
file(READ ${TINYEMU_DIR}/VERSION TINYEMU_VERSION)
string(STRIP "${TINYEMU_VERSION}" TINYEMU_VERSION)

# Create static library
add_library(tinyemu STATIC ${TINYEMU_SOURCES} ${TINYEMU_SLIRP_SOURCES})

# RISC-V CPU variants - need to compile riscv_cpu.c multiple times with different MAX_XLEN
# Use custom commands to generate the object files

set(RISCV_CPU_SRC ${TINYEMU_DIR}/riscv_cpu.c)

# riscv_cpu32.c - wrapper that sets MAX_XLEN=32
set(RISCV_CPU32_WRAPPER ${CMAKE_CURRENT_BINARY_DIR}/tinyemu_riscv_cpu32.c)
file(WRITE ${RISCV_CPU32_WRAPPER} "#define MAX_XLEN 32\n#include \"${RISCV_CPU_SRC}\"\n")

# riscv_cpu64.c - wrapper that sets MAX_XLEN=64
set(RISCV_CPU64_WRAPPER ${CMAKE_CURRENT_BINARY_DIR}/tinyemu_riscv_cpu64.c)
file(WRITE ${RISCV_CPU64_WRAPPER} "#define MAX_XLEN 64\n#include \"${RISCV_CPU_SRC}\"\n")

# riscv_cpu128.c - wrapper that sets MAX_XLEN=128 (requires int128 support)
set(RISCV_CPU128_WRAPPER ${CMAKE_CURRENT_BINARY_DIR}/tinyemu_riscv_cpu128.c)
file(WRITE ${RISCV_CPU128_WRAPPER} "#define MAX_XLEN 128\n#include \"${RISCV_CPU_SRC}\"\n")

target_sources(tinyemu PRIVATE
    ${RISCV_CPU32_WRAPPER}
    ${RISCV_CPU64_WRAPPER}
    ${RISCV_CPU128_WRAPPER}
)

# Include directories - set up so <tinyemu/...> works
# Create a wrapper include directory structure
set(TINYEMU_INCLUDE_WRAPPER ${CMAKE_CURRENT_BINARY_DIR}/tinyemu-include)
file(MAKE_DIRECTORY ${TINYEMU_INCLUDE_WRAPPER}/tinyemu)

# Symlink or copy headers to tinyemu subdirectory
set(TINYEMU_HEADERS
    cutils.h
    iomem.h
    virtio.h
    machine.h
    json.h
    fs.h
    list.h
    fbuf.h
    softfp.h
    riscv_cpu.h
    pci.h
    elf.h
    smp.h
    ydebug.h
)

foreach(HEADER ${TINYEMU_HEADERS})
    if(EXISTS ${TINYEMU_DIR}/${HEADER})
        configure_file(${TINYEMU_DIR}/${HEADER} ${TINYEMU_INCLUDE_WRAPPER}/tinyemu/${HEADER} COPYONLY)
    endif()
endforeach()

# SLIRP headers
file(MAKE_DIRECTORY ${TINYEMU_INCLUDE_WRAPPER}/tinyemu/slirp)
if(EXISTS ${TINYEMU_DIR}/slirp/libslirp.h)
    configure_file(${TINYEMU_DIR}/slirp/libslirp.h ${TINYEMU_INCLUDE_WRAPPER}/tinyemu/slirp/libslirp.h COPYONLY)
endif()

target_include_directories(tinyemu PUBLIC
    ${TINYEMU_INCLUDE_WRAPPER}
)

target_include_directories(tinyemu PRIVATE
    ${TINYEMU_DIR}
    ${TINYEMU_DIR}/slirp
)

# Compile definitions
target_compile_definitions(tinyemu PRIVATE
    _FILE_OFFSET_BITS=64
    _LARGEFILE_SOURCE
    CONFIG_VERSION="${TINYEMU_VERSION}"
    CONFIG_SLIRP
    CONFIG_RISCV_MAX_XLEN=128
)

# Platform-specific definitions
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_compile_definitions(tinyemu PRIVATE
        CONFIG_IOS
        _DARWIN_C_SOURCE
    )
elseif(APPLE)
    target_compile_definitions(tinyemu PRIVATE
        _DARWIN_C_SOURCE
    )
else()
    target_compile_definitions(tinyemu PRIVATE
        _GNU_SOURCE
    )
endif()

# Suppress warnings in third-party code
target_compile_options(tinyemu PRIVATE
    -Wno-deprecated-declarations
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-sign-compare
)

# Link libraries
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT APPLE)
    # Linux needs -lrt for clock functions
    target_link_libraries(tinyemu PRIVATE rt)
endif()

message(STATUS "TinyEMU: RISC-V emulator v${TINYEMU_VERSION}")

# Standalone temu executable for testing
add_executable(temu ${TINYEMU_DIR}/temu.c)
target_link_libraries(temu PRIVATE tinyemu)
target_include_directories(temu PRIVATE ${TINYEMU_DIR} ${TINYEMU_DIR}/slirp)
target_compile_definitions(temu PRIVATE
    _FILE_OFFSET_BITS=64
    _LARGEFILE_SOURCE
    CONFIG_VERSION="${TINYEMU_VERSION}"
    CONFIG_SLIRP
    CONFIG_RISCV_MAX_XLEN=128
    _GNU_SOURCE
)
