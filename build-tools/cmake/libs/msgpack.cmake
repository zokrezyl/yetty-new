# msgpack — MessagePack serialization (C only).
#
# yetty's only consumer is src/yetty/yrpc/rpc-message.c which uses the C
# API (`msgpack_unpacked`, `msgpack_unpack_next`, `msgpack_object`). We
# therefore pull only the pure-C library; the C++ branch (msgpack-cxx)
# was dropped to avoid a no-value C++ dependency.
if(TARGET msgpack-c)
    return()
endif()

CPMAddPackage(
    NAME msgpack-c
    GITHUB_REPOSITORY msgpack/msgpack-c
    GIT_TAG c-6.1.0
    OPTIONS
        "MSGPACK_BUILD_DOCS OFF"
        "MSGPACK_BUILD_TESTS OFF"
        "MSGPACK_BUILD_EXAMPLES OFF"
)

# Alias the static target under a stable name regardless of upstream renames.
if(NOT TARGET msgpack-c)
    add_library(msgpack-c ALIAS msgpackc-static)
endif()
