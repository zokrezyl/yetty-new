#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""check-trivy.py — Aqua Trivy filesystem vulnerability scan.

Runs `trivy fs` against the repo. Detects packages from a wide range of
manifests (lockfiles, OS package metadata, Cargo, npm, go.mod, etc.) and
reports CVEs from its vendored database.

Build-tree and tmp/ paths are skipped — they pull in transitive deps that
aren't ours to fix.

Exit codes:
  0  no vulnerabilities found in scope
  1  vulnerabilities reported
  2  trivy not installed / scan failed
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
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

# Trivy --skip-dirs accepts comma-separated globs. Keep these in sync with
# check-osv-scanner.py / check-grype.py.
DEFAULT_SKIPS = (
    "build-*",
    "tmp",
    ".git",
    "src/libvterm-0.3.3",  # vendored upstream — track separately if needed
    "src/tinyemu",
)
SEVERITY_ORDER = ("CRITICAL", "HIGH", "MEDIUM", "LOW", "UNKNOWN")


def check_trivy() -> CheckResult:
    started = time.time()
    binary = os.environ.get("TRIVY") or pick_tool("trivy")
    if not binary:
        err("trivy not installed")
        err("install: see https://aquasecurity.github.io/trivy/latest/getting-started/installation/")
        return CheckResult(name="trivy", status="fail",
                           summary="trivy not installed",
                           duration_s=time.time() - started)

    json_out = TMP_DIR / "trivy.json"
    log_out = TMP_DIR / "trivy.log"

    cmd = [
        binary, "fs",
        "--scanners", "vuln",
        "--format", "json",
        "--output", str(json_out),
        "--quiet",
        "--skip-dirs", ",".join(DEFAULT_SKIPS),
        str(REPO_ROOT),
    ]
    info(f"using {binary}")
    info(f"scanning: {rel(REPO_ROOT)} (skipping {', '.join(DEFAULT_SKIPS)})")
    proc = run(cmd)
    log_out.write_text((proc.stdout or "") + (proc.stderr or ""))
    if proc.returncode != 0:
        err(f"trivy exited rc={proc.returncode}")
        err(f"see {rel(log_out)}")
        return CheckResult(name="trivy", status="fail",
                           summary=f"trivy failed (rc={proc.returncode})",
                           log_path=log_out,
                           duration_s=time.time() - started)
    if not json_out.exists() or json_out.stat().st_size == 0:
        ok("no vulnerabilities (no JSON output produced)")
        return CheckResult(name="trivy", status="pass",
                           summary="no vulns",
                           duration_s=time.time() - started)

    try:
        data = json.loads(json_out.read_text())
    except json.JSONDecodeError as e:
        err(f"trivy JSON parse error: {e}")
        return CheckResult(name="trivy", status="fail",
                           summary="invalid JSON",
                           log_path=json_out,
                           duration_s=time.time() - started)

    sev_counts: Counter = Counter()
    pkg_counts: Counter = Counter()
    findings: list[str] = []

    for tgt in data.get("Results", []) or []:
        target = tgt.get("Target", "?")
        rel_target = target.replace(str(REPO_ROOT) + "/", "")
        # Defense in depth: even if --skip-dirs missed something, filter again.
        if any(rel_target.startswith(p.rstrip("*")) for p in DEFAULT_SKIPS):
            continue
        for v in tgt.get("Vulnerabilities", []) or []:
            sev = (v.get("Severity") or "UNKNOWN").upper()
            sev_counts[sev] += 1
            pkg = v.get("PkgName", "?")
            pkg_counts[pkg] += 1
            ver = v.get("InstalledVersion", "?")
            fix = v.get("FixedVersion", "")
            vid = v.get("VulnerabilityID", "?")
            line = f"{sev:<8} {vid:<22} {pkg}@{ver}"
            if fix:
                line += f"  fix>={fix}"
            line += f"  ({rel_target})"
            findings.append(line)

    total = sum(sev_counts.values())
    findings.sort(key=lambda s: SEVERITY_ORDER.index(s.split()[0])
                  if s.split() and s.split()[0] in SEVERITY_ORDER else 99)
    (TMP_DIR / "trivy.findings.txt").write_text(
        "\n".join(findings) + ("\n" if findings else "")
    )
    (TMP_DIR / "trivy.summary.txt").write_text(
        "\n".join(f"{s:<10} {sev_counts.get(s, 0)}" for s in SEVERITY_ORDER
                  if sev_counts.get(s)) + "\n"
    )

    if total == 0:
        ok("no vulnerabilities in scope")
        return CheckResult(name="trivy", status="pass",
                           summary="no vulns",
                           duration_s=time.time() - started)
    warn(f"{total} vulnerability finding(s)")
    return CheckResult(
        name="trivy", status="issues",
        summary=f"{total} finding(s) across {len(pkg_counts)} package(s)",
        details=["severities: " + ", ".join(f"{s}({sev_counts[s]})"
                                            for s in SEVERITY_ORDER if sev_counts.get(s))],
        counts={"total": total, **dict(sev_counts)},
        log_path=TMP_DIR / "trivy.findings.txt",
        duration_s=time.time() - started,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.parse_args()
    return check_trivy().exit_code


if __name__ == "__main__":
    sys.exit(main())
