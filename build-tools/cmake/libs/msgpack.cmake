# msgpack - MessagePack serialization
if(TARGET msgpack-cxx)
    return()
endif()

# C++ header-only library
CPMAddPackage(
    NAME msgpack-cxx
    GITHUB_REPOSITORY msgpack/msgpack-c
    GIT_TAG cpp-7.0.0
    OPTIONS
        "MSGPACK_BUILD_DOCS OFF"
        "MSGPACK_BUILD_TESTS OFF"
        "MSGPACK_BUILD_EXAMPLES OFF"
        "MSGPACK_USE_BOOST OFF"
)

# C library (needed for yrpc)
CPMAddPackage(
    NAME msgpack-c
    GITHUB_REPOSITORY msgpack/msgpack-c
    GIT_TAG c-6.1.0
    OPTIONS
        "MSGPACK_BUILD_DOCS OFF"
        "MSGPACK_BUILD_TESTS OFF"
        "MSGPACK_BUILD_EXAMPLES OFF"
)

# Create alias for the C library target (always static)
if(NOT TARGET msgpack-c)
    add_library(msgpack-c ALIAS msgpackc-static)
endif()
