#!/usr/bin/env python3
"""One-shot migration: insert top/bottom_color_penetration_layers into every
U1 process preset, mirroring min(product default, effective shell layers)
of each preset (inherits chain resolved). Text-level insertion keeps the original
file formatting byte-identical outside the two inserted lines.

Review decision (MR #15): only U1 presets carry explicit values; other machines
fall back to the PrintConfig code defaults, so their JSONs stay untouched."""
import json
import os
import re
import sys

PROFILE_DIR = os.path.join(os.path.dirname(__file__), "..", "resources", "profiles", "Snapmaker", "process")
TOP_DEFAULT = 5     # product requirement: 0.4mm nozzle, 0.2mm layer height
BOTTOM_DEFAULT = 3
# Code-level defaults in PrintConfig.cpp, used when a root preset carries no
# explicit shell layers at all (e.g. abstract chain roots like fdm_process_U1).
TOP_SHELL_FALLBACK = 4
BOTTOM_SHELL_FALLBACK = 3


def load_all():
    # Load the full preset graph: inherits chains of U1 presets run through
    # non-U1 bases (e.g. fdm_process_common), so resolution needs them all.
    by_name, paths = {}, {}
    for fn in os.listdir(PROFILE_DIR):
        if not fn.endswith(".json"):
            continue
        p = os.path.join(PROFILE_DIR, fn)
        d = json.load(open(p, encoding="utf-8"))
        by_name[d["name"]] = d
        paths[d["name"]] = p
    return by_name, paths


def effective(d, by_name, key, seen):
    """Resolve a key along the inherits chain (child overrides parent)."""
    if d.get(key) is not None:
        return d[key]
    parent = d.get("inherits")
    if parent and parent in by_name and parent not in seen:
        seen.add(parent)
        return effective(by_name[parent], by_name, key, seen)
    return None


def main():
    by_name, paths = load_all()
    changed, skipped = 0, 0
    report = []
    for name, d in sorted(by_name.items()):
        # Review decision (MR #15): only U1 presets carry explicit values;
        # other machines fall back to the PrintConfig code defaults.
        if "U1" not in os.path.basename(paths[name]):
            continue
        top_shell = effective(d, by_name, "top_shell_layers", {name})
        bottom_shell = effective(d, by_name, "bottom_shell_layers", {name})
        if top_shell is None:
            top_shell, report_line = TOP_SHELL_FALLBACK, f" (top shell fell back to code default {TOP_SHELL_FALLBACK})"
        else:
            report_line = ""
        if bottom_shell is None:
            bottom_shell = BOTTOM_SHELL_FALLBACK
            report_line += f" (bottom shell fell back to code default {BOTTOM_SHELL_FALLBACK})"
        top_pen = min(TOP_DEFAULT, int(top_shell))
        bottom_pen = min(BOTTOM_DEFAULT, int(bottom_shell))
        p = paths[name]
        with open(p, encoding="utf-8", newline="") as f:
            text = f.read()
        nl = "\r\n" if "\r\n" in text else "\n"

        def insert_after(text, anchor_key, new_key, new_val):
            # Idempotency first: never insert a key that is already present.
            if '"%s"' % new_key in text:
                return text, True
            # CRLF files keep \r at line end; allow it before $ so the anchor matches
            pat = re.compile(r'^( *)("%s": *"[^"]*",)[ \t\r]*$' % re.escape(anchor_key), re.M)
            m = pat.search(text)
            line = m and (m.group(1) + '"%s": "%s",' % (new_key, new_val))
            if m:
                return text[: m.end(2)] + nl + line + text[m.end(2):], True
            return text, False

        text, ok_top = insert_after(text, "top_shell_layers", "top_color_penetration_layers", top_pen)
        text, ok_bottom = insert_after(text, "bottom_shell_layers", "bottom_color_penetration_layers", bottom_pen)
        # Files without explicit shell layers keys (base presets): insert after the
        # mandatory "from" metadata line so both keys land in a stable position.
        if not ok_top:
            text, ok_top = insert_after(text, "from", "top_color_penetration_layers", top_pen)
        if not ok_bottom:
            text, ok_bottom = insert_after(text, "from", "bottom_color_penetration_layers", bottom_pen)
        if not ok_top or not ok_bottom:
            skipped += 1
            report.append(f"SKIP (no insertion anchor): {name}")
            continue
        with open(p, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        changed += 1
        report.append(f"OK   {name}: shell {top_shell}/{bottom_shell} -> penetration {top_pen}/{bottom_pen}")
    print("\n".join(report))
    print(f"\nchanged={changed} skipped={skipped}")
    return 0 if skipped == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
