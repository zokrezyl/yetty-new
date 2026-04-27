#!/usr/bin/env bash
# Audit which 3rdparty libs have their tarballs published on GitHub
# releases vs which platforms each lib's build.sh declares it should
# target. Prints a lib × platform matrix:
#
#   ✓   tarball present in release
#   ✗   build.sh accepts this platform but no tarball published (FAIL)
#   ·   build.sh excludes this platform (n/a — not expected)
#   ?   tag missing entirely on remote (no release at all)
#
# Each cell links to the release page for fast drill-down.
#
# Usage:
#   build-tools/gh/check-lib-build-artifacts.sh
#   build-tools/gh/check-lib-build-artifacts.sh libssh2 msdfgen   # subset
#   REMOTE_OWNER=zokrezyl REMOTE_REPO=yetty ./check-lib-build-artifacts.sh
#
# Implemented in Python (inline) — easier to read than bash for the
# release/asset matching logic. Run via `uv run` so the rich dep is
# pulled on demand without polluting the system Python.

set -euo pipefail

if ! command -v uv >/dev/null 2>&1; then
    echo "error: uv not found. Install: https://docs.astral.sh/uv/" >&2
    exit 1
fi
if ! command -v gh >/dev/null 2>&1; then
    echo "error: gh (GitHub CLI) not found." >&2
    exit 1
fi

cd "$(dirname "$0")/../.."

REMOTE_OWNER="${REMOTE_OWNER:-zokrezyl}"
REMOTE_REPO="${REMOTE_REPO:-yetty}"

exec uv run --quiet --with rich python3 - "$REMOTE_OWNER" "$REMOTE_REPO" "$@" <<'PY'
"""Cross-check 3rdparty release artifacts against producer build.sh declarations."""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

from rich.console import Console
from rich.table import Table


# Canonical platform order for the matrix columns. tvOS columns reflect
# qemu's matrix (native arm64 device + x86_64 simulator on Intel macOS).
ALL_PLATFORMS = [
    "linux-x86_64",
    "linux-aarch64",
    "macos-x86_64",
    "macos-arm64",
    "ios-arm64",
    "ios-x86_64",
    "tvos-arm64",
    "tvos-x86_64",
    "android-arm64-v8a",
    "android-x86_64",
    "webasm",
    "windows-x86_64",
]

REPO_ROOT = Path.cwd()
THREEPARTY_DIR = REPO_ROOT / "build-tools" / "3rdparty"


def gh_release_assets(owner: str, repo: str, tag: str) -> list[str] | None:
    """Asset filenames for a release, or None if the release doesn't exist."""
    proc = subprocess.run(
        ["gh", "release", "view", tag, "--repo", f"{owner}/{repo}",
         "--json", "assets"],
        capture_output=True, text=True,
    )
    if proc.returncode != 0:
        # Distinguish "no such release" from other failures.
        if "release not found" in proc.stderr.lower() or "not found" in proc.stderr.lower():
            return None
        # Other gh failure — bubble up so the user sees it (auth, rate limit, …).
        sys.stderr.write(f"gh release view {tag} failed:\n{proc.stderr}")
        return None
    data = json.loads(proc.stdout)
    return [a["name"] for a in data.get("assets", [])]


def parse_supported_platforms(build_sh: Path) -> set[str] | None:
    """Extract supported targets from a producer's `build.sh` `case` block.

    Returns None for noarch producers (any platform yields the same
    single tarball). Returns the set of declared platforms otherwise.

    A platform listed in a branch whose body REJECTS it (e.g. libco's
    `webasm) ... exit 1 ;;`) is NOT counted. Detection: branches whose
    only meaningful action is exit / error are treated as rejection;
    branches that set SHELL_NAME or exec into _build.sh are accepted.
    """
    if (build_sh.parent / ".noarch").exists():
        return None
    text = build_sh.read_text()
    m = re.search(
        r'case\s+"\$TARGET_PLATFORM"\s+in(.*?)esac',
        text, re.DOTALL,
    )
    if not m:
        return set()
    body = m.group(1)

    # Strip bash line-continuations so multi-line patterns collapse to a
    # single line: `linux-x86_64|...|\\\n    macos-x86_64|...)` becomes
    # `linux-x86_64|...|    macos-x86_64|...)` — fine for whole-word match.
    body_flat = re.sub(r'\\\s*\n', ' ', body)

    # Split on the `;;` branch terminator; for each segment extract the
    # `pattern) body` halves on the FIRST unparenthesised `)`.
    found: set[str] = set()
    for raw_branch in body_flat.split(';;'):
        if not raw_branch.strip():
            continue
        # Split off the leading whitespace — patterns start at the first
        # non-whitespace char after the previous `;;`.
        m = re.match(r'\s*(?P<pat>[^)]+)\)(?P<body>.*)', raw_branch, re.DOTALL)
        if not m:
            continue
        pat = m.group('pat')
        b = m.group('body')
        # Skip the catch-all `*)` branch.
        if pat.strip() == '*':
            continue
        # Reject branches: body has `exit 1` (or non-zero exit) AND no
        # SHELL_NAME assignment AND no exec into _build.sh.
        is_accept = (
            re.search(r'\bSHELL_NAME\s*=', b)
            or re.search(r'exec\s+bash\b.*_build\.sh', b, re.DOTALL)
        )
        is_reject = bool(re.search(r'\bexit\s+1\b', b)) and not is_accept
        if is_reject:
            continue
        for plat in ALL_PLATFORMS:
            if re.search(rf'\b{re.escape(plat)}\b', pat):
                found.add(plat)
    return found


def lib_meta(lib_dir: Path) -> dict | None:
    """Read a lib's metadata. Returns None if the dir isn't a producer."""
    ver_file = lib_dir / "version"
    if not ver_file.is_file():
        return None
    version = ver_file.read_text().strip()
    if not version:
        return None
    build_sh = lib_dir / "build.sh"
    supported = parse_supported_platforms(build_sh) if build_sh.exists() else set()
    is_noarch = supported is None
    return {
        "name": lib_dir.name,
        "version": version,
        "is_noarch": is_noarch,
        "supported": supported if supported is not None else set(),
        "tag": f"lib-{lib_dir.name}-{version}",
    }


def expected_asset_names(lib: dict) -> dict[str, str]:
    """Map of platform → expected tarball filename. For noarch libs, the
    single asset is mapped under a synthetic 'noarch' key."""
    if lib["is_noarch"]:
        return {"noarch": f"{lib['name']}-{lib['version']}.tar.gz"}
    return {
        plat: f"{lib['name']}-{plat}-{lib['version']}.tar.gz"
        for plat in lib["supported"]
    }


def main() -> int:
    owner, repo = sys.argv[1], sys.argv[2]
    only = set(sys.argv[3:])

    libs: list[dict] = []
    for d in sorted(THREEPARTY_DIR.iterdir()):
        if not d.is_dir():
            continue
        if only and d.name not in only:
            continue
        meta = lib_meta(d)
        if meta:
            libs.append(meta)

    console = Console()
    if not libs:
        console.print("[yellow]no libs matched[/]")
        return 0

    table = Table(title=f"3rdparty release artifacts — {owner}/{repo}",
                  show_lines=False, header_style="bold")
    table.add_column("lib", no_wrap=True)
    table.add_column("version", no_wrap=True, style="dim")
    for plat in ALL_PLATFORMS:
        table.add_column(plat, justify="center", no_wrap=True)
    table.add_column("tag", no_wrap=True, style="dim")

    summary_ok = summary_missing = summary_no_tag = 0

    for lib in libs:
        row = [lib["name"], lib["version"]]
        assets = gh_release_assets(owner, repo, lib["tag"])
        expected = expected_asset_names(lib)

        if assets is None:
            # No release at all.
            for _plat in ALL_PLATFORMS:
                # Even the no-tag case respects supported set —
                # n/a (·) for excluded targets, ? for missing-tag.
                row.append("[red]?[/]" if (lib["is_noarch"] or _plat in lib["supported"]) else "[dim]·[/]")
            row.append(f"[red]{lib['tag']} (no release)[/]")
            summary_no_tag += 1
        elif lib["is_noarch"]:
            present = expected["noarch"] in assets
            for plat in ALL_PLATFORMS:
                # Noarch: only the linux-x86_64 column gets the marker;
                # the rest are "covered by noarch" — show ✓ everywhere
                # if asset present, else ✗.
                row.append("[green]✓[/]" if present else "[red]✗[/]")
            if present:
                summary_ok += 1
            else:
                summary_missing += 1
            row.append(lib["tag"])
        else:
            any_missing = False
            for plat in ALL_PLATFORMS:
                if plat in lib["supported"]:
                    if expected[plat] in assets:
                        row.append("[green]✓[/]")
                    else:
                        row.append("[red]✗[/]")
                        any_missing = True
                else:
                    row.append("[dim]·[/]")
            if any_missing:
                summary_missing += 1
            else:
                summary_ok += 1
            row.append(lib["tag"])
        table.add_row(*row)

    console.print(table)
    console.print(
        f"\n[green]complete[/]: {summary_ok}    "
        f"[red]missing assets[/]: {summary_missing}    "
        f"[red]no release[/]: {summary_no_tag}"
    )
    console.print(
        "\nlegend: [green]✓[/] published   [red]✗[/] expected but missing   "
        "[red]?[/] tag has no release   [dim]·[/] platform not supported by this lib"
    )
    return 1 if (summary_missing + summary_no_tag) else 0


if __name__ == "__main__":
    raise SystemExit(main())
PY
