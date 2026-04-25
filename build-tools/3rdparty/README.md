# 3rdparty — pre-built 3rd-party libraries

Mirrors the `build-tools/assets/` group, but for **3rd-party libraries that
yetty links against**. Built on GitHub Actions and published as tarballs
attached to a `3rdparty-X.Y.Z` release; downstream yetty builds fetch the
tarballs instead of rebuilding from source on every CI run / developer
machine.

## Versioning model — bump-rebuilds-everything

There is **one** version, pinned in `/3rdparty.version` (next to
`assets.version`). The CI workflow is triggered by a `3rdparty-X.Y.Z` tag
and rebuilds **every library × every target platform** as a single
release. Per-library versions are NOT independent.

When to bump `3rdparty.version`:
- a library tag in `versions.txt` is updated, OR
- a new library is added, OR
- a `_build.sh` patch needs republishing.

Yetty's CMake reads `3rdparty.version` to know which release tag to fetch
the tarballs from — same mechanism as `assets-fetch.cmake`.

## Layout

```
build-tools/3rdparty/
├── README.md           — this file
├── versions.txt        — single source of truth (lib → upstream tag)
├── <lib>/
│   ├── build.sh        — nix-shell wrapper, picks .#3rdparty-<target>
│   └── _build.sh       — actual build, reads TARGET_PLATFORM/VERSION/OUTPUT_DIR
└── ...
```

Tarball naming: `<lib>-<target-platform>-<version>.tar.gz`, layout
`{lib,lib64}/...` + `include/...` (drop-in for the imported-static-target
pattern already used by `Dawn.cmake` / `Dav1d.cmake`).

## Target platforms

Same set as `assets/qemu/` (link targets only — no `tvos-arm64`):

| target              | runner            | nix shell                    |
|---------------------|-------------------|------------------------------|
| `linux-x86_64`      | ubuntu-latest     | `.#3rdparty-linux-x86_64`     |
| `linux-aarch64`     | ubuntu-latest     | `.#3rdparty-linux-aarch64`    |
| `android-arm64-v8a` | ubuntu-latest     | `.#3rdparty-android-arm64-v8a`|
| `android-x86_64`    | ubuntu-latest     | `.#3rdparty-android-x86_64`   |
| `webasm`            | ubuntu-latest     | `.#3rdparty-webasm`           |
| `macos-arm64`       | macos-latest      | `.#3rdparty-macos-arm64`      |
| `ios-arm64`         | macos-latest      | `.#3rdparty-ios-arm64`        |
| `macos-x86_64`      | macos-15-intel    | `.#3rdparty-macos-x86_64`     |
| `ios-x86_64`        | macos-15-intel    | `.#3rdparty-ios-x86_64`       |
| `windows-x86_64`    | windows-latest    | (no nix — PowerShell + MSVC) |

## Tier list of yetty 3rd-party deps

Tier 1 = always-from-source heavyweights, biggest CI wins.
Tier 2 = moderate, worth bundling for cross-platform pain.
Tier 3 = header-only / trivial — keep as CPM source, no pre-build.
Tier 0 = already pre-built (Dawn).

| Tier | Library          | Build system       | Status |
|------|------------------|--------------------|--------|
| 0    | dawn             | (pre-built)        | done — `Dawn.cmake` |
| 1    | **openh264**     | GNU make           | **scaffolded (this PR)** |
| 1    | dav1d            | meson + ninja      | todo   |
| 1    | openssl          | CMake (janbar)     | todo   |
| 1    | freetype-bundle  | CMake (5 libs)     | todo — zlib + libpng + brotli + bzip2 + freetype |
| 1    | libmagic         | autotools          | todo   |
| 1    | thorvg           | CMake              | todo (carries our patches) |
| 1    | libjpeg-turbo    | CMake + NASM       | todo   |
| 1    | libssh2          | CMake              | todo (chains on openssl/zlib) |
| 1    | libcurl          | CMake / autotools  | todo (currently system-only) |
| 2    | libuv            | CMake              | todo   |
| 2    | vterm            | CMake              | todo   |
| 2    | wasm3            | CMake              | todo   |
| 2    | tree-sitter (+14 grammars) | CMake    | todo (one bundled tarball) |
| 2    | pdfio            | autotools          | todo   |
| 2    | tinyxml2 / yaml-cpp / libyaml | CMake | todo   |
| 3    | args, glm, stb, msgpack-cxx, libco, incbin, lz4, spdlog, ytrace, msdfgen, minimp4, miniaudio, imgui | (header-only / single-file) | skip — keep CPM |

## Adding a new library

1. Add a row to `versions.txt`.
2. `mkdir build-tools/3rdparty/<lib> && cp openh264/{build,_build}.sh ./` and
   adapt the wrapper + _build.sh.
3. Add a job to `.github/workflows/build-3rdparty.yml` (matrix copy of the
   library above).
4. Replace the `*.cmake` in `build-tools/cmake/` with a thin
   imported-static-target stub that pulls the tarball via
   `3rdparty-fetch.cmake` (mirror of `assets-fetch.cmake`).
5. **Bump `/3rdparty.version`** and push a `3rdparty-X.Y.Z` tag — the CI
   workflow rebuilds the whole matrix (every lib × every target).

## Two-version-files reality

`build-tools/nix/versions.txt` pins upstream sources for **CPM-from-source
nix builds** (alternate path). `build-tools/3rdparty/versions.txt` pins
upstream tags for the **prebuilt tarballs**. Both must agree on the tag for
each library — until we unify them.
