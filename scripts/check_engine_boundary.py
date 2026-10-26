#!/usr/bin/env python3
"""Engine-layer boundary checker.

Dual-layer check driven by scripts/engine_boundary_config.json (single
source of truth):

  1. include layer : engine source files must not include anything under
                     slic3r/GUI/ (C/C++ comments are stripped before matching,
                     so mentions inside comments do not count)
  2. link layer    : engine CMake targets must not link wx / GUI-side targets
                     (structured target_link_libraries parse), plus a raw
                     keyword fallback scan of pure-engine CMake files

Engine baseline is zero violations, so the check is a full scan: any finding
fails the gate. Local runs and CI use this same script.

Usage:
  python scripts/check_engine_boundary.py [--config PATH] [--warning]

Exit code 0 = clean (or warning-only with --warning), 1 = violations found.
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG = Path(__file__).resolve().parent / "engine_boundary_config.json"

SCOPE_KEYWORDS = {"public", "private", "interface", "link_public", "link_private",
                  "debug", "optimized", "general"}


def norm_path(path: Path) -> str:
    """Repo-relative, forward-slash path for config/allowlist comparison."""
    return path.resolve().relative_to(REPO_ROOT).as_posix()


def strip_comments(text: str) -> str:
    """Blank out // and /* */ comments while preserving line structure, so
    line numbers stay valid and commented-out includes never match."""
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


def check_include_layer(cfg):
    """Return (violations, empty_dirs).

    violations: (file, line, matched_include)
    empty_dirs: engine dirs with zero scannable source files - a path typo or
    a moved directory would otherwise make the gate pass silently.
    """
    sub = cfg["include_check"]["forbidden_substring"]
    exts = set(cfg["include_check"]["scan_extensions"])
    allow = set(cfg.get("allowlist", []))
    violations, empty_dirs = [], []
    for entry in cfg["engine_dirs"]:
        root = REPO_ROOT / entry["path"]
        if not root.is_dir():
            print(f"ERROR: engine dir not found: {entry['path']}", file=sys.stderr)
            sys.exit(2)
        scanned = 0
        for path in sorted(root.rglob("*")):
            if not path.is_file() or path.suffix.lower() not in exts:
                continue
            scanned += 1
            rel = norm_path(path)
            if rel in allow:
                print(f"[allowlist] skipped {rel} (entry in allowlist)")
                continue
            text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            for m in INCLUDE_RE.finditer(text):
                target = m.group(1)
                if sub in target:
                    line = text.count("\n", 0, m.start()) + 1
                    violations.append((rel, line, target))
        if scanned == 0:
            empty_dirs.append(entry["path"])
    return violations, empty_dirs


def iter_link_blocks(text: str):
    """Yield (start_line, target_token, block_inner) per target_link_libraries call."""
    for m in re.finditer(r"\btarget_link_libraries\s*\(", text):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        inner = text[m.end():i - 1]
        line = text.count("\n", 0, m.start()) + 1
        tokens = inner.split()
        if tokens:
            yield line, tokens[0], tokens[1:]


def check_link_layer(cfg):
    """Return (violations, unmatched_specs, matched_blocks).

    unmatched_specs: specs whose target name matched no target_link_libraries
    block in their cmake_file. Zero matches means config drift (target renamed
    or moved) would make the link gate pass silently, so it is a hard error.
    """
    patterns = [re.compile(p, re.IGNORECASE) for p in cfg["link_check"]["forbidden_link_patterns"]]
    violations, unmatched = [], []
    matched_blocks = 0
    for spec in cfg["link_check"]["targets"]:
        cmake = REPO_ROOT / spec["cmake_file"]
        if not cmake.is_file():
            print(f"ERROR: CMake file not found: {spec['cmake_file']}", file=sys.stderr)
            sys.exit(2)
        text = cmake.read_text(encoding="utf-8", errors="replace")
        found = False
        for line, target, tokens in iter_link_blocks(text):
            if target != spec["name"]:
                continue
            found = True
            matched_blocks += 1
            for tok in tokens:
                core = tok[2:-1] if tok.startswith("${") and tok.endswith("}") else tok
                hits = [pat.pattern for pat in patterns if pat.search(core)]
                if hits:
                    violations.append((spec["cmake_file"], line, target, tok, ", ".join(hits)))
        if not found:
            unmatched.append(spec)
    return violations, unmatched, matched_blocks


def check_raw_layer(cfg) -> list:
    """Belt-and-braces raw keyword scan of pure-engine CMake files."""
    patterns = [re.compile(p, re.IGNORECASE) for p in cfg["link_check"]["forbidden_raw_patterns"]]
    violations = []
    for rel in cfg["link_check"].get("raw_scan_files", []):
        cmake = REPO_ROOT / rel
        if not cmake.is_file():
            print(f"ERROR: CMake file not found: {rel}", file=sys.stderr)
            sys.exit(2)
        for lineno, line in enumerate(
                cmake.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            hits = [pat.pattern for pat in patterns if pat.search(line)]
            if hits:
                violations.append((rel, lineno, "-", line.strip(), ", ".join(hits)))
    return violations


def main() -> int:
    ap = argparse.ArgumentParser(description="Engine-layer boundary checker")
    ap.add_argument("--config", type=Path, default=DEFAULT_CONFIG,
                    help=f"boundary config (default: {DEFAULT_CONFIG.name})")
    ap.add_argument("--warning", action="store_true",
                    help="report violations as warnings, exit 0 (emergency rollback flag)")
    args = ap.parse_args()

    cfg = json.loads(args.config.read_text(encoding="utf-8"))

    include_v, empty_dirs = check_include_layer(cfg)
    link_v, unmatched, matched_blocks = check_link_layer(cfg)
    raw_v = check_raw_layer(cfg)

    # Gate integrity errors: the gate itself would silently pass. Never
    # downgraded by --warning (that flag only softens real violations).
    if empty_dirs or unmatched:
        for d in empty_dirs:
            print(f"ERROR: engine dir '{d}' contains no scannable source files.\n"
                  f"  The include-level gate would silently pass for it. Fix the path "
                  f"in scripts/engine_boundary_config.json.", file=sys.stderr)
        for spec in unmatched:
            print(f"ERROR: target '{spec['name']}' has no target_link_libraries block "
                  f"in {spec['cmake_file']}.\n"
                  f"  The link-level gate would silently pass for it. Fix "
                  f"scripts/engine_boundary_config.json (target renamed/moved?) "
                  f"or the CMake file.", file=sys.stderr)
        print("check_engine_boundary: gate integrity check failed (exit 2)", file=sys.stderr)
        return 2

    for f, line, inc in include_v:
        print(f"[engine-boundary] include-layer violation\n"
              f"  {f}:{line}: #include \"{inc}\"\n"
              f"  fix: engine code MUST NOT depend on slic3r/GUI (see "
              f"scripts/engine_boundary_config.json); remove the include or file an "
              f"allowlist entry after review")
    for f, line, target, tok, pat in link_v:
        print(f"[engine-boundary] link-layer violation\n"
              f"  {f}:{line}: target {target} links forbidden '{tok}' (pattern '{pat}')\n"
              f"  fix: engine targets MUST NOT link wx/GUI libraries; remove the "
              f"linkage or file an allowlist entry after review")
    for f, line, _, snippet, pat in raw_v:
        print(f"[engine-boundary] raw-keyword violation (fallback scan)\n"
              f"  {f}:{line}: '{snippet}' matches '{pat}'\n"
              f"  fix: engine CMake files MUST NOT reference wx/GUI; remove it or "
              f"file an allowlist entry after review")

    total = len(include_v) + len(link_v) + len(raw_v)
    if total:
        summary = (f"check_engine_boundary: {total} violation(s) "
                   f"({len(include_v)} include, {len(link_v)} link, {len(raw_v)} raw)")
        print(summary + ("  [WARNING MODE: not failing]" if args.warning else ""))
        return 0 if args.warning else 1

    print("OK: engine boundary clean (baseline 0 maintained; "
          f"{len(cfg['engine_dirs'])} engine dirs, "
          f"{matched_blocks} link block(s) across "
          f"{len(cfg['link_check']['targets'])} target(s) checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
