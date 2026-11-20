#!/usr/bin/env python3
"""Widgets decoupling gate (KR3): src/widgets must not reference libslic3r_gui.

Include-direction check only: any `#include ... slic3r/GUI/...` under
src/widgets is a layering violation (the dependency direction is
libslic3r_gui -> widgets). C/C++ comments are stripped before matching, so
mentions inside comments never count.

Transition semantics differ from the engine gate: the allowlist
(scripts/widgets_gui_allowlist.txt) exempts a HEADER everywhere in
src/widgets, one path per line, to be deleted as includes get extracted
away. Allowlist hits are reported visibly so reviewers can track them.

Same CLI contract as check_engine_boundary.py:
  python scripts/check_widgets_deps.py [--config PATH] [--warning]
Exit codes: 0 = clean (or warning-only with --warning), 1 = violations, 2 = setup error.
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG = Path(__file__).resolve().parent / "widgets_boundary_config.json"


def norm_path(path: Path) -> str:
    """Repo-relative, forward-slash path for reporting."""
    return path.resolve().relative_to(REPO_ROOT).as_posix()


def strip_comments(text: str) -> str:
    """Blank out // and /* */ comments while preserving line structure, so
    line numbers stay valid and commented-out includes never match.

    Known limitation: string literals are not recognized, so a "/*" inside a
    string can open a phantom block comment and hide subsequent includes
    (conservative direction - it can only under-report, never false-positive).
    """
    out = []
    i, n = 0, len(text)
    state = "code"
    while i < n:
        two = text[i:i + 2]
        if state == "code":
            if two == "//":
                state = "line"
                i += 2
            elif two == "/*":
                state = "block"
                i += 2
            else:
                out.append(text[i])
                i += 1
        elif state == "line":
            if text[i] == "\n":
                state = "code"
                out.append("\n")
            i += 1
        else:  # block
            if two == "*/":
                state = "code"
                i += 2
            else:
                if text[i] == "\n":
                    out.append("\n")
                i += 1
    return "".join(out)


INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]([^\">]+)", re.MULTILINE)


def load_allowlist(path: Path) -> set:
    """Non-empty, non-comment lines = exact include paths (e.g. slic3r/GUI/Gui.hpp)."""
    if not path.is_file():
        print(f"ERROR: allowlist not found: {path}", file=sys.stderr)
        sys.exit(2)
    entries = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            entries.add(line)
    return entries


def main() -> int:
    ap = argparse.ArgumentParser(description="Widgets decoupling gate (KR3)")
    ap.add_argument("--config", type=Path, default=DEFAULT_CONFIG,
                    help=f"boundary config (default: {DEFAULT_CONFIG.name})")
    ap.add_argument("--warning", action="store_true",
                    help="report violations as warnings, exit 0 (emergency rollback flag)")
    args = ap.parse_args()

    cfg = json.loads(args.config.read_text(encoding="utf-8"))
    sub = cfg["include_check"]["forbidden_substring"]
    exts = set(cfg["include_check"]["scan_extensions"])
    allow = load_allowlist(REPO_ROOT / cfg["allowlist_file"])

    watch = REPO_ROOT / cfg["watch_dir"]
    if not watch.is_dir():
        print(f"ERROR: watch dir not found: {cfg['watch_dir']}", file=sys.stderr)
        sys.exit(2)

    violations = []
    scanned = 0
    for path in sorted(watch.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in exts:
            continue
        scanned += 1
        rel = norm_path(path)
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for m in INCLUDE_RE.finditer(text):
            target = m.group(1)
            if sub not in target:
                continue
            line = text.count("\n", 0, m.start()) + 1
            if target in allow:
                print(f"[allowlist] skipped {rel}:{line}: {target}")
                continue
            violations.append((rel, line, target))

    for rel, line, target in violations:
        print(f"[widgets-boundary] include-layer violation\n"
              f"  {rel}:{line}: #include \"{target}\"\n"
              f"  fix: src/widgets MUST NOT depend on libslic3r_gui; extract the "
              f"needed code into src/widgets, or (transition only, review required) "
              f"add '{target}' to {cfg['allowlist_file']} and remove it later")

    if violations:
        summary = f"check_widgets_deps: {len(violations)} violation(s) in {cfg['watch_dir']}"
        print(summary + ("  [WARNING MODE: not failing]" if args.warning else ""))
        return 0 if args.warning else 1

    print(f"OK: widgets deps clean ({scanned} files scanned in {cfg['watch_dir']}; "
          f"no non-allowlisted slic3r/GUI includes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
