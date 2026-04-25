#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""check-grype.py — Anchore Grype filesystem vulnerability scan.

Runs `grype dir:<repo>` against the repo. Coverage overlaps with trivy
but uses a different vulnerability database (NVD + GitHub advisories +
distro-specific feeds), so running both gives more confidence.

Build-tree and tmp/ paths are excluded.

Exit codes:
  0  no vulnerabilities found in scope
  1  vulnerabilities reported
  2  grype not installed / scan failed
"""

from __future__ import annotations

import argparse
import json
import os
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
    ok,
    pick_tool,
    rel,
    run,
    warn,
)

# grype --exclude takes glob expressions, repeated. Patterns are matched
# against the path relative to the scanned root.
DEFAULT_EXCLUDES = (
    "./build-*/**",
    "./tmp/**",
    "./.git/**",
    "./src/libvterm-0.3.3/**",
    "./src/tinyemu/**",
)
SEVERITY_ORDER = ("Critical", "High", "Medium", "Low", "Negligible", "Unknown")


def check_grype() -> CheckResult:
    started = time.time()
    binary = os.environ.get("GRYPE") or pick_tool("grype")
    if not binary:
        err("grype not installed")
        err("install: curl -sSfL https://raw.githubusercontent.com/anchore/"
            "grype/main/install.sh | sh -s -- -b ~/.local/bin")
        return CheckResult(name="grype", status="fail",
                           summary="grype not installed",
                           duration_s=time.time() - started)

    json_out = TMP_DIR / "grype.json"
    log_out = TMP_DIR / "grype.log"

    cmd = [binary, f"dir:{REPO_ROOT}", "--output", "json", "--quiet"]
    for pat in DEFAULT_EXCLUDES:
        cmd.extend(["--exclude", pat])

    info(f"using {binary}")
    info(f"scanning: {rel(REPO_ROOT)} (excluding build-*, tmp/, vendored deps)")
    proc = run(cmd)
    json_out.write_text(proc.stdout or "")
    log_out.write_text(proc.stderr or "")
    if proc.returncode != 0 or not (proc.stdout or "").strip():
        err(f"grype exited rc={proc.returncode}")
        err(f"see {rel(log_out)}")
        return CheckResult(name="grype", status="fail",
                           summary=f"grype failed (rc={proc.returncode})",
                           log_path=log_out,
                           duration_s=time.time() - started)

    try:
        data = json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        err(f"grype JSON parse error: {e}")
        return CheckResult(name="grype", status="fail",
                           summary="invalid JSON",
                           log_path=json_out,
                           duration_s=time.time() - started)

    sev_counts: Counter = Counter()
    pkg_counts: Counter = Counter()
    findings: list[str] = []

    for m in data.get("matches", []) or []:
        v = m.get("vulnerability", {}) or {}
        a = m.get("artifact", {}) or {}
        sev = v.get("severity", "Unknown")
        sev_counts[sev] += 1
        pkg = a.get("name", "?")
        pkg_counts[pkg] += 1
        ver = a.get("version", "?")
        vid = v.get("id", "?")
        fix_versions = (v.get("fix") or {}).get("versions") or []
        fix = ",".join(fix_versions[:3]) if fix_versions else ""
        loc = ""
        locs = a.get("locations") or []
        if locs:
            loc = locs[0].get("path", "")
            loc = loc.replace(str(REPO_ROOT), "").lstrip("/")
        line = f"{sev:<10} {vid:<22} {pkg}@{ver}"
        if fix:
            line += f"  fix>={fix}"
        if loc:
            line += f"  ({loc})"
        findings.append(line)

    total = sum(sev_counts.values())
    findings.sort(key=lambda s: SEVERITY_ORDER.index(s.split()[0])
                  if s.split() and s.split()[0] in SEVERITY_ORDER else 99)
    (TMP_DIR / "grype.findings.txt").write_text(
        "\n".join(findings) + ("\n" if findings else "")
    )
    (TMP_DIR / "grype.summary.txt").write_text(
        "\n".join(f"{s:<12} {sev_counts.get(s, 0)}" for s in SEVERITY_ORDER
                  if sev_counts.get(s)) + "\n"
    )

    if total == 0:
        ok("no vulnerabilities in scope")
        return CheckResult(name="grype", status="pass",
                           summary="no vulns",
                           duration_s=time.time() - started)
    warn(f"{total} vulnerability finding(s)")
    return CheckResult(
        name="grype", status="issues",
        summary=f"{total} finding(s) across {len(pkg_counts)} package(s)",
        details=["severities: " + ", ".join(f"{s}({sev_counts[s]})"
                                            for s in SEVERITY_ORDER if sev_counts.get(s))],
        counts={"total": total, **dict(sev_counts)},
        log_path=TMP_DIR / "grype.findings.txt",
        duration_s=time.time() - started,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.parse_args()
    return check_grype().exit_code


if __name__ == "__main__":
    sys.exit(main())
