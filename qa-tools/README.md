# qa-tools

Small, focused shell wrappers around C static-analysis and formatting tools.
Each script does one thing; `qa-overview.sh` runs them all and prints a
single-screen summary.

All logs and reports go under `tmp/qa/` at the repo root.

## Scripts

| Script                  | What it does                                                | Tool               |
|-------------------------|-------------------------------------------------------------|--------------------|
| `check-format.sh`       | Reports files that don't match `.clang-format`              | `clang-format`     |
| `apply-format.sh`       | Rewrites files in place to match `.clang-format`            | `clang-format`     |
| `check-clang-tidy.sh`   | Runs `clang-tidy` across sources using a compile database   | `clang-tidy`       |
| `check-cppcheck.sh`     | Runs `cppcheck` with a useful default check set             | `cppcheck`         |
| `check-scan-build.sh`   | Builds under `scan-build` (Clang Static Analyzer)           | `scan-build`, `cmake` |
| `qa-overview.sh`        | Runs each check, prints a pass/issues/fail table            | (orchestrator)     |

## Scope

By default the scripts only look at our own code:

- `src/yetty/`
- `src/yrender-utils/`
- `include/yetty/`

Third-party directories (`src/libvterm-0.3.3/`, `src/tinyemu/`) are always
excluded.

Limit the scope for a single run with `QA_PATHS`:

```sh
QA_PATHS="src/yetty/ygui" qa-tools/check-format.sh
```

## Requirements

- `clang-format` — any recent version (`clang-format-9` works).
- `clang-tidy` — version 14+ recommended; needs a `compile_commands.json`.
  Build once (e.g. `make build-desktop-ytrace-release`), then the script
  auto-discovers the DB in `build-*/`. Override with `QA_BUILD_DIR=<dir>`.
- `scan-build` — from `clang-tools-<ver>`; the script drives its own cmake
  build under `tmp/qa/scan-build-build/`.
- `cppcheck` — any recent version.

## Typical workflow

```sh
# First-time: normalize whitespace across the tree.
qa-tools/check-format.sh       # see what would change
qa-tools/apply-format.sh       # rewrite in place (refuses on a dirty tree)

# Ongoing: run the full overview after changes.
make build-desktop-ytrace-release   # produces compile_commands.json
qa-tools/qa-overview.sh
```

Exit codes are consistent across scripts:

- `0` — clean
- `1` — check ran, reported issues
- `2` — check could not run (missing tool, missing compile DB, etc.)

## Adding a new check

1. Drop a new `check-<thing>.sh` into this directory.
2. `source lib/common.sh` for paths, logging, and the shared file discovery.
3. Write detailed output under `${QA_TMP_DIR}` and a short human summary to
   stdout, using the same exit-code convention.
4. Add a row to the `checks` array in `qa-overview.sh`.
