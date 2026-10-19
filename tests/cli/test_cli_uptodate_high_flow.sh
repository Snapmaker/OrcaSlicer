#!/usr/bin/env bash
# --uptodate on a project with "CLI HF PETG" (Standard 255 / High Flow 265) and "CLI HF PLA" (210):
# PETG keeps its High Flow column and value, PLA its value, filament_self_index one entry per column.
# usage: test_cli_uptodate_high_flow.sh <snapmaker-orca binary> <python3> <source dir>
set -u

BIN="${1:-}"
PY="${2:-python3}"
SRC="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: snapmaker-orca binary not found: $BIN"; exit 77; }
[ -f "$SRC/src/libslic3r/PrintConfig.cpp" ] || { echo "SKIP: source dir not found: $SRC"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-uptodate-hf.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/datadir"

# with_timeout <seconds> <command...>: coreutils timeout where present, else an alarm through perl.
with_timeout() {
    local seconds="$1"; shift
    if command -v timeout > /dev/null 2>&1; then
        timeout "$seconds" "$@"
    else
        perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"
    fi
}

# Standalone presets: without "inherits" the CLI loads them as-is, with no preset bundle.
cat > "$WORK/machine.json" <<'EOF'
{
    "type": "machine",
    "from": "User",
    "name": "CLI uptodate test printer",
    "printable_area": ["0x0", "200x0", "200x200", "0x200"],
    "printable_height": "100",
    "layer_change_gcode": "G92 E0"
}
EOF
cat > "$WORK/process.json" <<'EOF'
{
    "type": "process",
    "from": "User",
    "name": "CLI uptodate test process",
    "enable_support": "0"
}
EOF
filament() { # filament <file> <name> <type> <temperature>
    cat > "$1" <<EOF
{
    "type": "filament",
    "from": "User",
    "name": "$2",
    "filament_type": ["$3"],
    "filament_extruder_variant": ["Direct Drive Standard"],
    "nozzle_temperature": ["$4"],
    "nozzle_temperature_initial_layer": ["$4"]
}
EOF
}
filament "$WORK/petg.json" "CLI HF PETG" "PETG" 255
filament "$WORK/pla.json" "CLI HF PLA" "PLA" 210

"$PY" - "$WORK/cube.stl" <<'EOF'
import sys
v = [(x, y, z) for z in (0, 10) for y in (0, 10) for x in (0, 10)]
with open(sys.argv[1], "w") as f:
    f.write("solid cube\n")
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        for tri in ((v[a], v[b], v[c]), (v[a], v[c], v[d])):
            f.write("facet normal 0 0 0\nouter loop\n")
            for p in tri:
                f.write("vertex %g %g %g\n" % p)
            f.write("endloop\nendfacet\n")
    f.write("endsolid cube\n")
EOF

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# 1. The project: both filaments with one column each.
mkdir -p "$WORK/first"
with_timeout 300 "$BIN" --datadir "$WORK/datadir" --load-settings "$WORK/machine.json;$WORK/process.json" \
    --load-filaments "$WORK/petg.json;$WORK/pla.json" --slice 0 --outputdir "$WORK/first" --export-3mf project.3mf \
    "$WORK/cube.stl" > "$WORK/first/log" 2>&1
status=$?
[ "$status" -eq 0 ] || { tail -n 30 "$WORK/first/log"; echo "FAIL: building the project exited with $status"; exit 1; }
[ -f "$WORK/first/project.3mf" ] || { echo "FAIL: no project.3mf written"; exit 1; }

# 2. PETG gains a High Flow column with a High Flow nozzle temperature of 265, listed as changed
#    against its preset, as the Filament tab saves it into a project.
"$PY" - "$SRC/src/libslic3r/PrintConfig.cpp" "$WORK/first/project.3mf" "$WORK/project_hf.3mf" <<'EOF' || { echo "FAIL: the project could not be given a High Flow column"; exit 1; }
import json, re, sys, zipfile

source, src_3mf, dst_3mf = sys.argv[1:4]
text = open(source, encoding="utf-8").read()
block = re.search(r"std::set<std::string> filament_options_with_variant = \{(.*?)\};", text, re.S).group(1)
keys = set(re.findall(r'^\s*"([a-z0-9_]+)"', block, re.M))

name = "Metadata/project_settings.config"
with zipfile.ZipFile(src_3mf) as z:
    entries = [(info, z.read(info.filename)) for info in z.infolist()]
config = json.loads(dict((info.filename, data) for info, data in entries)[name])

variants = config["filament_extruder_variant"]
if variants != ["Direct Drive Standard", "Direct Drive Standard"]:
    sys.exit("unexpected filament_extruder_variant %r" % variants)
# Every per-column key of PETG (filament 1, column 0) gets a High Flow column as a copy of it.
width = len(variants)
for key in keys:
    value = config.get(key)
    if isinstance(value, list) and len(value) == width:
        value.insert(1, value[0])
config["filament_extruder_variant"] = ["Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Standard"]
config["nozzle_temperature"][1] = "265"
config["filament_self_index"] = ["1", "1", "2"]
different = config.get("different_settings_to_system") or ["", "", "", ""]
if len(different) != 4:
    sys.exit("unexpected different_settings_to_system %r" % different)
different[1] = "filament_extruder_variant;nozzle_temperature"
config["different_settings_to_system"] = different

with zipfile.ZipFile(dst_3mf, "w", zipfile.ZIP_DEFLATED) as z:
    for info, data in entries:
        z.writestr(info, json.dumps(config, indent=4) if info.filename == name else data)
EOF

# 3. The refresh, from files with one column each.
mkdir -p "$WORK/refresh"
with_timeout 300 "$BIN" --datadir "$WORK/datadir" --uptodate --uptodate-filaments "$WORK/petg.json;$WORK/pla.json" \
    --slice 0 --outputdir "$WORK/refresh" --export-3mf project.3mf "$WORK/project_hf.3mf" > "$WORK/refresh/log" 2>&1
status=$?
if [ "$status" -ne 0 ]; then
    tail -n 30 "$WORK/refresh/log"
    fail "--uptodate exited with $status"
elif [ ! -f "$WORK/refresh/project.3mf" ]; then
    fail "--uptodate wrote no project.3mf"
else
    "$PY" - "$WORK/refresh/project.3mf" <<'EOF' || fail "the refreshed project lost a column or a value"
import json, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    config = json.loads(z.read("Metadata/project_settings.config"))
ok = True
def expect(key, wanted):
    global ok
    if config.get(key) != wanted:
        print("%s = %r, expected %r" % (key, config.get(key), wanted))
        ok = False
expect("filament_extruder_variant", ["Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Standard"])
expect("nozzle_temperature", ["255", "265", "210"])
expect("filament_self_index", ["1", "1", "2"])
sys.exit(0 if ok else 1)
EOF
fi

if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed"
    exit 1
fi
echo "PASS"
