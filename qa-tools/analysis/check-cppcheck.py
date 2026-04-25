#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""check-cppcheck.py — run cppcheck against our C sources.

Produces a structured log and returns a CheckResult with:
  counts: total, error, warning, style, performance, portability, information
  details: top rule IDs, error-severity findings (for the report)
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _common import (  # noqa: E402
    REPO_ROOT,
    TMP_DIR,
    CheckResult,
    err,
    info,
    list_sources,
    ok,
    pick_tool,
    rel,
    run,
    scope_paths_from_env,
    warn,
)

SEVERITIES = ("error", "warning", "style", "performance", "portability", "information")
DIAG_LINE_RE = re.compile(
    r"^(?P<file>[^:]+):(?P<line>\d+):(?P<col>\d+): (?P<sev>\w+): (?P<msg>.*) \[(?P<rule>[A-Za-z0-9_]+)\]$"
)


def check_cppcheck(paths: list[str] | None = None) -> CheckResult:
    started = time.time()
    binary = os.environ.get("CPPCHECK") or pick_tool("cppcheck")
    if not binary:
        err("cppcheck not installed")
        return CheckResult(name="cppcheck", status="fail",
                           summary="cppcheck not installed",
                           duration_s=time.time() - started)

    files = list_sources(paths)
    if not files:
        warn("no source files found")
        return CheckResult(name="cppcheck", status="pass",
                           summary="no files", duration_s=time.time() - started)

    log_path = TMP_DIR / "cppcheck.log"
    enable = os.environ.get("QA_CPPCHECK_ENABLE",
                            "warning,style,performance,portability")
    jobs = os.environ.get("QA_CPPCHECK_JOBS", str(os.cpu_count() or 4))

    info(f"using {binary}")
    cmd = [
        binary,
        f"--enable={enable}",
        "--inline-suppr",
        "--quiet",
        "--std=c11",
        "--language=c",
        "-I", str(REPO_ROOT / "include"),
        "-j", jobs,
        "--template={file}:{line}:{column}: {severity}: {message} [{id}]",
    ]
    cmd.extend(str(f) for f in files)

    # cppcheck writes diagnostics to stderr; always capture both streams.
    proc = run(cmd)
    log_text = proc.stderr or ""
    log_path.write_text(log_text)

    lines = [ln for ln in log_text.splitlines() if ln.strip()]
    counts: Counter = Counter()
    rule_counts: Counter = Counter()
    per_file: Counter = Counter()
    errors: list[str] = []

    for ln in lines:
        m = DIAG_LINE_RE.match(ln)
        if not m:
            continue
        sev = m.group("sev")
        counts[sev] += 1
        rule_counts[m.group("rule")] += 1
        per_file[rel(m.group("file"))] += 1
        if sev == "error":
            errors.append(f"{rel(m.group('file'))}:{m.group('line')}  {m.group('msg')} [{m.group('rule')}]")

    total = sum(counts.values())
    (TMP_DIR / "cppcheck.summary.txt").write_text(
        "\n".join(f"{s:<14}{counts.get(s, 0)}" for s in SEVERITIES) + "\n"
    )
    (TMP_DIR / "cppcheck.top-rules.txt").write_text(
        "\n".join(f"{n:6d}  {r}" for r, n in rule_counts.most_common()) + "\n"
    )
    (TMP_DIR / "cppcheck.per-file.txt").write_text(
        "\n".join(f"{n:6d}  {f}" for f, n in per_file.most_common()) + "\n"
    )

    details = []
    if counts:
        by_sev = ", ".join(f"{s}={counts[s]}" for s in SEVERITIES if counts.get(s))
        details.append(f"severities: {by_sev}")
    if rule_counts:
        details.append("top rules: "
                       + ", ".join(f"{r}({n})" for r, n in rule_counts.most_common(5)))
    if errors:
        details.append(f"{len(errors)} error(s) — see cppcheck.log")

    summary = f"{total} diagnostic(s)" if total else "no diagnostics"
    if total:
        warn(summary)
        warn(f"log: {rel(log_path)}")
        status = "issues"
    else:
        ok(summary)
        status = "pass"

    return CheckResult(
        name="cppcheck", status=status, summary=summary, details=details,
        counts={"total": total, **dict(counts)},
        log_path=log_path, duration_s=time.time() - started,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="*", help="Optional scope (repo-relative).")
    args = ap.parse_args()
    paths = args.paths or scope_paths_from_env()
    return check_cppcheck(paths).exit_code


if __name__ == "__main__":
    sys.exit(main())
