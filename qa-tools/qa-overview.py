#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""qa-overview.py — run every check and print a real report.

Each section answers: what is wrong, where, and how bad.
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import sys
from pathlib import Path

import importlib.util

_here = Path(__file__).resolve().parent
sys.path.insert(0, str(_here))
from _common import (  # noqa: E402
    REPO_ROOT,
    TMP_DIR,
    C,
    CheckResult,
    info,
    scope_paths_from_env,
)


def _load(stem: str, attr: str):
    """Load a sibling hyphenated script as a module and return one of its attrs."""
    path = _here / f"{stem}.py"
    spec = importlib.util.spec_from_file_location(stem.replace("-", "_"), path)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return getattr(mod, attr)


check_format = _load("check-format", "check_format")
check_cppcheck = _load("check-cppcheck", "check_cppcheck")
check_clang_tidy = _load("check-clang-tidy", "check_clang_tidy")
check_scan_build = _load("check-scan-build", "check_scan_build")

RULE = "=" * 72


def status_color(st: str) -> str:
    return {
        "pass": C.GREEN,
        "issues": C.YELLOW,
        "fail": C.RED,
        "skip": C.DIM,
    }.get(st, "")


def banner(title: str) -> None:
    print(RULE)
    print(f" {title}")
    print(RULE)


def section(res: CheckResult) -> None:
    col = status_color(res.status)
    print()
    print(f"{C.BOLD}[{res.name}]{C.RESET}  {col}{res.status}{C.RESET}  "
          f"({res.duration_s:.1f}s)  {res.summary}")


def dump_top(path: Path, n: int, indent: str = "    ") -> None:
    if not path.exists():
        return
    for line in path.read_text().splitlines()[:n]:
        print(f"{indent}{line}")


# ---------------------------------------------------------------- format detail
def dump_format(res: CheckResult) -> None:
    per_file = TMP_DIR / "check-format.per-file.txt"
    if not per_file.exists() or per_file.stat().st_size == 0:
        return
    print()
    print("  Worst-formatted files (changed lines, top 10):")
    dump_top(per_file, 10)
    # Roll up by module = first three path segments.
    modules: dict[str, int] = {}
    for ln in per_file.read_text().splitlines():
        parts = ln.strip().split(None, 1)
        if len(parts) != 2:
            continue
        n_str, path = parts
        try:
            n = int(n_str)
        except ValueError:
            continue
        segs = path.split("/")
        mod = "/".join(segs[:3]) if len(segs) >= 3 else path
        modules[mod] = modules.get(mod, 0) + n
    if modules:
        print()
        print("  Worst-formatted modules:")
        for mod, n in sorted(modules.items(), key=lambda kv: -kv[1])[:8]:
            print(f"    {n:6d}  {mod}")
    print()
    print("  Fix:  qa-tools/apply-format.py")
    print(f"  Full diff:  tmp/qa/check-format.diff")


# ---------------------------------------------------------------- cppcheck detail
def dump_cppcheck(res: CheckResult) -> None:
    summary = TMP_DIR / "cppcheck.summary.txt"
    if summary.exists() and summary.stat().st_size > 0:
        print()
        print("  By severity:")
        for ln in summary.read_text().splitlines():
            parts = ln.split()
            if len(parts) == 2 and parts[1] != "0":
                print(f"    {parts[0]:<14} {parts[1]}")

    top_rules = TMP_DIR / "cppcheck.top-rules.txt"
    if top_rules.exists():
        print()
        print("  Top rules:")
        dump_top(top_rules, 8)

    # Read the log to pull error-severity lines verbatim — this is what the
    # user actually wants to see.
    log = TMP_DIR / "cppcheck.log"
    if log.exists():
        errs = [ln for ln in log.read_text().splitlines() if ": error: " in ln]
        if errs:
            print()
            print(f"  {C.RED}ERRORS ({len(errs)}) — fix these first:{C.RESET}")
            repo = str(REPO_ROOT) + "/"
            for ln in errs:
                print(f"     {ln.replace(repo, '')}")

    per_file = TMP_DIR / "cppcheck.per-file.txt"
    if per_file.exists():
        print()
        print("  Files with most diagnostics (top 10):")
        dump_top(per_file, 10)

    print()
    print(f"  Full log:  tmp/qa/cppcheck.log")


# ---------------------------------------------------------------- clang-tidy detail
def dump_clang_tidy(res: CheckResult) -> None:
    if res.status == "fail":
        for d in res.details:
            print(f"  {d}")
        print("  Hint: run 'make build-desktop-ytrace-release' first,")
        print("        or set QA_BUILD_DIR=<dir with compile_commands.json>.")
        return
    top = TMP_DIR / "clang-tidy.top-rules.txt"
    if top.exists():
        print()
        print("  Top checks:")
        dump_top(top, 10)
    per_file = TMP_DIR / "clang-tidy.summary.txt"
    if per_file.exists():
        print()
        print("  Files with most diagnostics (top 10):")
        dump_top(per_file, 10)
    print()
    print(f"  Full log:  tmp/qa/clang-tidy.log")


# ---------------------------------------------------------------- scan-build detail
def dump_scan_build(res: CheckResult) -> None:
    for d in res.details:
        print(f"  {d}")


# ---------------------------------------------------------------- main
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--skip", action="append", default=[],
                    help="Skip a check by name: format, tidy, cppcheck, scan-build.")
    args = ap.parse_args()
    skip = set(args.skip) | set(
        s for s in os.environ.get("QA_SKIP", "").split() if s
    )

    paths = scope_paths_from_env()
    scope_text = " ".join(paths) if paths else "src/yetty src/yrender-utils include/yetty"

    banner(f"QA REPORT — {dt.datetime.now():%Y-%m-%d %H:%M:%S}")
    print(f" scope: {scope_text}")
    if skip:
        print(f" skipped: {' '.join(sorted(skip))}")
    print(RULE)

    results: list[tuple[CheckResult, callable]] = []

    def run_one(name: str, fn, detail_fn):
        if name in skip:
            res = CheckResult(name=name, status="skip", summary="skipped")
        else:
            info(f"running {name}...")
            res = fn()
        results.append((res, detail_fn))

    run_one("format",     lambda: check_format(paths),     dump_format)
    run_one("tidy",       lambda: check_clang_tidy(paths), dump_clang_tidy)
    run_one("cppcheck",   lambda: check_cppcheck(paths),   dump_cppcheck)
    run_one("scan-build", lambda: check_scan_build(),      dump_scan_build)

    for res, detail_fn in results:
        section(res)
        if res.status in ("issues", "fail"):
            detail_fn(res)

    print()
    print(RULE)
    parts = []
    for res, _ in results:
        col = status_color(res.status)
        parts.append(f"{res.name}={col}{res.status}{C.RESET}")
    print(" SUMMARY: " + "  ".join(parts))
    print(RULE)

    overall = 0
    for res, _ in results:
        overall = max(overall, res.exit_code)
    return overall


if __name__ == "__main__":
    sys.exit(main())
