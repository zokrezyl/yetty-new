"""Shared helpers for qa-tools/check-*.py scripts."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TMP_DIR = REPO_ROOT / "tmp" / "qa"
TMP_DIR.mkdir(parents=True, exist_ok=True)

# Directories we consider "our" code. Third-party trees are always excluded.
DEFAULT_SOURCE_ROOTS = (
    "src/yetty",
    "src/yrender-utils",
    "include/yetty",
)
EXCLUDE_PATH_FRAGMENTS = (
    "/libvterm-0.3.3/",
    "/tinyemu/",
)


# ---------------------------------------------------------------- output
_ISATTY = sys.stdout.isatty()


class C:
    RESET = "\033[0m" if _ISATTY else ""
    BOLD = "\033[1m" if _ISATTY else ""
    DIM = "\033[2m" if _ISATTY else ""
    RED = "\033[31m" if _ISATTY else ""
    GREEN = "\033[32m" if _ISATTY else ""
    YELLOW = "\033[33m" if _ISATTY else ""
    BLUE = "\033[34m" if _ISATTY else ""
    CYAN = "\033[36m" if _ISATTY else ""


def info(msg: str) -> None:
    print(f"{C.BLUE}[qa]{C.RESET} {msg}")


def ok(msg: str) -> None:
    print(f"{C.GREEN}[ok]{C.RESET} {msg}")


def warn(msg: str) -> None:
    print(f"{C.YELLOW}[warn]{C.RESET} {msg}")


def err(msg: str) -> None:
    print(f"{C.RED}[err]{C.RESET} {msg}", file=sys.stderr)


# ---------------------------------------------------------------- discovery
def list_sources(paths: list[str] | None = None) -> list[Path]:
    """Return absolute paths of C/H files under our source roots."""
    roots_rel = paths if paths else list(DEFAULT_SOURCE_ROOTS)
    files: list[Path] = []
    for rel in roots_rel:
        root = (REPO_ROOT / rel).resolve()
        if not root.exists():
            continue
        for p in root.rglob("*"):
            if not p.is_file():
                continue
            if p.suffix not in (".c", ".h"):
                continue
            s = str(p)
            if any(frag in s for frag in EXCLUDE_PATH_FRAGMENTS):
                continue
            files.append(p)
    files.sort()
    return files


def rel(path: Path | str) -> str:
    """Path relative to repo root, as a string. Falls back to absolute."""
    p = Path(path)
    try:
        return str(p.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(p)


def pick_tool(*candidates: str) -> str | None:
    """Return the first binary from `candidates` that's on PATH."""
    for c in candidates:
        if shutil.which(c):
            return c
    return None


def scope_paths_from_env() -> list[str] | None:
    """Read QA_PATHS env var — space-separated list of repo-relative paths."""
    raw = os.environ.get("QA_PATHS", "").strip()
    return raw.split() if raw else None


# ---------------------------------------------------------------- subprocess
def run(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    capture: bool = True,
    check: bool = False,
) -> subprocess.CompletedProcess:
    """Thin wrapper around subprocess.run with sensible defaults."""
    return subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        capture_output=capture,
        text=True,
        check=check,
    )


# ---------------------------------------------------------------- result shape
@dataclass
class CheckResult:
    """Uniform shape every check returns. Overview consumes this."""

    name: str
    status: str  # "pass" | "issues" | "fail" | "skip"
    summary: str = ""
    details: list[str] = field(default_factory=list)
    counts: dict[str, int] = field(default_factory=dict)
    log_path: Path | None = None
    duration_s: float = 0.0

    @property
    def exit_code(self) -> int:
        return {"pass": 0, "issues": 1, "fail": 2, "skip": 0}[self.status]
