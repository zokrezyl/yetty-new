# libyaml - YAML 1.1 parser and emitter
if(TARGET yaml)
    return()
endif()

CPMAddPackage(
    NAME libyaml
    GITHUB_REPOSITORY yaml/libyaml
    GIT_TAG 0.2.5
    OPTIONS
        "BUILD_TESTING OFF"
        "BUILD_SHARED_LIBS OFF"
        "INSTALL_CMAKE_DIR OFF"
)
