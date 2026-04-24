# Custom rules for third-party cmake libraries

def cmake_library(
    name,
    url,
    source_dir,
    static_lib,
    include_dir = "include",
    cmake_args = "",
    exported_linker_flags = [],
    deps = [],
    visibility = ["PUBLIC"]):
    """
    Build a cmake-based library and make it available as a prebuilt library.
    """
    build_name = name + "_build"

    native.genrule(
        name = build_name,
        out = "install",
        bash = """
            set -e
            WORK="$TMPDIR/{name}-work"
            mkdir -p "$WORK"
            cd "$WORK"
            curl -fsSL "{url}" | tar -xz
            cmake -S {source_dir} -B build \\
                -DCMAKE_BUILD_TYPE=Release \\
                -DCMAKE_INSTALL_PREFIX="$OUT" \\
                -DCMAKE_POSITION_INDEPENDENT_CODE=ON \\
                -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \\
                {cmake_args}
            cmake --build build --parallel
            cmake --install build
        """.format(
            name = name,
            url = url,
            source_dir = source_dir,
            cmake_args = cmake_args,
        ),
        cacheable = True,
        visibility = ["PUBLIC"],
    )

    native.prebuilt_cxx_library(
        name = name,
        static_lib = ":{build_name}[{static_lib}]".format(
            build_name = build_name,
            static_lib = static_lib,
        ),
        exported_headers = ":{build_name}[{include_dir}]".format(
            build_name = build_name,
            include_dir = include_dir,
        ),
        exported_linker_flags = exported_linker_flags,
        deps = deps,
        visibility = visibility,
    )
