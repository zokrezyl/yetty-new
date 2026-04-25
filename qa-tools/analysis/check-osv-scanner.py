#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""check-osv-scanner.py — scan dependency manifests for known CVEs.

Runs `osv-scanner scan source` against the repo. Detects packages via
their lockfiles (Cargo.lock, package-lock.json, go.mod, requirements.txt,
flake.lock, etc.) and queries the OSV database for known vulnerabilities.

Note: osv-scanner is lockfile-based. Vendored C source trees (libvterm,
tinyemu) are invisible to it. Coverage of those needs an SBOM with
explicit CPEs — out of scope here.

Exit codes:
  0  no vulnerabilities found
  1  vulnerabilities reported
  2  osv-scanner not installed / scan failed
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

# Skip vendored / generated trees: the build directories pull in transitive
# deps (Jinja2 in libuv docs, etc.) that aren't ours to fix.
DEFAULT_SKIPS = (
    "build-*",
    "tmp",
    ".git",
    "src/libvterm-0.3.3",
    "src/tinyemu",
)


def check_osv_scanner() -> CheckResult:
    started = time.time()
    binary = os.environ.get("OSV_SCANNER") or pick_tool("osv-scanner")
    if not binary:
        err("osv-scanner not installed")
        err("install: curl -fsSL -o ~/.local/bin/osv-scanner "
            "https://github.com/google/osv-scanner/releases/latest/download/"
            "osv-scanner_linux_amd64 && chmod +x ~/.local/bin/osv-scanner")
        return CheckResult(name="osv-scanner", status="fail",
                           summary="osv-scanner not installed",
                           duration_s=time.time() - started)

    json_out = TMP_DIR / "osv-scanner.json"
    log_out = TMP_DIR / "osv-scanner.log"

    cmd = [binary, "scan", "source", "--recursive", "--format", "json"]
    for pat in DEFAULT_SKIPS:
        cmd.extend(["--paths-relative-to-scan-dir"])  # noop guard for older versions
        break  # only need to set this once
    # Newer osv-scanner uses --offline-vulnerabilities/--paths-relative; for
    # exclusion we rely on --recursive + .osv-scanner.toml. Keep it simple:
    # scan the repo root and rely on lockfile discovery to skip uninteresting
    # dirs. Build-tree noise is filtered post-hoc.
    cmd = [binary, "scan", "source", "--recursive", "--format", "json", str(REPO_ROOT)]

    info(f"using {binary}")
    info(f"scanning: {rel(REPO_ROOT)} (excluding build-*, tmp/)")
    proc = run(cmd)
    json_out.write_text(proc.stdout or "")
    log_out.write_text(proc.stderr or "")

    # osv-scanner exits 1 when vulns are found, 0 when clean, 128 when no
    # lockfiles are detected. Parse before deciding status.
    if not proc.stdout.strip():
        if proc.returncode == 128 or "No package sources found" in (proc.stderr or ""):
            ok("no lockfiles found — nothing to scan")
            return CheckResult(name="osv-scanner", status="pass",
                               summary="no lockfiles in scope",
                               log_path=log_out,
                               duration_s=time.time() - started)
        err(f"osv-scanner produced no JSON (rc={proc.returncode})")
        err(f"see {rel(log_out)}")
        return CheckResult(name="osv-scanner", status="fail",
                           summary=f"scan failed (rc={proc.returncode})",
                           log_path=log_out,
                           duration_s=time.time() - started)

    try:
        data = json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        err(f"osv-scanner JSON parse error: {e}")
        return CheckResult(name="osv-scanner", status="fail",
                           summary="invalid JSON output",
                           log_path=json_out,
                           duration_s=time.time() - started)

    sev_counts: Counter = Counter()
    pkg_counts: Counter = Counter()
    findings: list[str] = []

    for result in data.get("results", []):
        src = result.get("source", {}).get("path", "")
        # Skip findings under build/tmp directories.
        rel_src = src.replace(str(REPO_ROOT) + "/", "")
        if any(rel_src.startswith(prefix.rstrip("*")) for prefix in DEFAULT_SKIPS):
            continue
        for pkg in result.get("packages", []):
            name = pkg.get("package", {}).get("name", "?")
            ver = pkg.get("package", {}).get("version", "?")
            for vuln in pkg.get("vulnerabilities", []):
                vid = vuln.get("id", "?")
                sev = "UNKNOWN"
                for s in vuln.get("severity", []):
                    sev = s.get("type", sev)
                    break
                # Try database_specific.severity for a string severity.
                ds = vuln.get("database_specific", {}) or {}
                if "severity" in ds:
                    sev = str(ds["severity"]).upper()
                sev_counts[sev] += 1
                pkg_counts[name] += 1
                findings.append(f"{sev:<10} {vid:<22} {name}@{ver}  ({rel_src})")

    total = sum(sev_counts.values())
    (TMP_DIR / "osv-scanner.findings.txt").write_text(
        "\n".join(findings) + ("\n" if findings else "")
    )
    (TMP_DIR / "osv-scanner.summary.txt").write_text(
        "\n".join(f"{s:<14} {n}" for s, n in sev_counts.most_common()) + "\n"
    )

    if total == 0:
        ok("no vulnerabilities in project lockfiles")
        return CheckResult(name="osv-scanner", status="pass",
                           summary="no vulns",
                           duration_s=time.time() - started)
    warn(f"{total} vulnerability finding(s)")
    return CheckResult(
        name="osv-scanner", status="issues",
        summary=f"{total} finding(s) across {len(pkg_counts)} package(s)",
        details=[f"top severities: " + ", ".join(f"{s}({n})"
                                                 for s, n in sev_counts.most_common(4))],
        counts={"total": total, **dict(sev_counts)},
        log_path=TMP_DIR / "osv-scanner.findings.txt",
        duration_s=time.time() - started,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.parse_args()
    return check_osv_scanner().exit_code


if __name__ == "__main__":
    sys.exit(main())
