"""Rebuild package excluding unnecessary build artifacts."""
import os, pathlib, zipfile, time

ROOT = pathlib.Path("C:/workDir/programing/SnapMakerOracaRequirements/OrcaSlicer")
BUILD_DIR = ROOT / "build" / "src" / "Release"
RESOURCES = ROOT / "resources"
TOOL_DIR = ROOT / "tools" / "cli_test_tool"
GCODE_DIFF = pathlib.Path("C:/workDir/programing/Gcode-diff/gcode-diff-rs/target/release/gcode-diff.exe")
ELECTRON_EXE = TOOL_DIR / "electron" / "dist" / "Snapmaker-CLI-Test-Tool-1.0.0-x64.exe"
OUTPUT = pathlib.Path("C:/workDir/programing/SnapMakerOracaRequirements/OrcaSlicer/tools/cli_test_tool/packaging/Snapmaker-CLI-Package-1.0.0.zip")

ESSENTIAL_RESOURCES = ["profiles", "calib", "printers", "fonts"]
EXCLUDE_EXTS = {".pdb", ".exp", ".lib"}  # build artifacts not needed at runtime

def should_include(f):
    return f.suffix.lower() not in EXCLUDE_EXTS

print("Rebuilding package (excluding .pdb, .exp, .lib)...")
t0 = time.time()

with zipfile.ZipFile(str(OUTPUT), "w", zipfile.ZIP_DEFLATED) as zf:
    # 1. CLI binary + runtime DLLs only (no .pdb/.exp/.lib)
    n_cli = 0
    for f in sorted(BUILD_DIR.iterdir()):
        if f.is_file() and should_include(f):
            zf.write(str(f), f.name)
            n_cli += 1
    print(f"  Added {n_cli} CLI/dll files")

    # 2. Essential resources
    for res in ESSENTIAL_RESOURCES:
        src = RESOURCES / res
        if src.exists():
            n_files = 0
            for f in sorted(src.rglob("*")):
                if f.is_file():
                    rel = f.relative_to(RESOURCES)
                    zf.write(str(f), str(rel))
                    n_files += 1
            print(f"  Added resources/{res}/ ({n_files} files)")
        else:
            print(f"  WARNING: resources/{res} not found")

    # 3. Test tool web app
    zf.write(str(TOOL_DIR / "app.py"), "tools/cli_test_tool/app.py")
    for f in sorted((TOOL_DIR / "templates").rglob("*")):
        if f.is_file():
            rel = f.relative_to(TOOL_DIR)
            zf.write(str(f), str(rel))
    print("  Added test tool (app.py + templates/)")

    # 3a. start.bat launcher
    start_bat = TOOL_DIR / "start.bat"
    if start_bat.exists():
        zf.write(str(start_bat), "tools/cli_test_tool/start.bat")
        print("  Added start.bat")
    readme = TOOL_DIR / "README.md"
    if readme.exists():
        zf.write(str(readme), "tools/cli_test_tool/README.md")
        print("  Added README.md")

    # 3b. gcode-diff quality scoring engine
    if GCODE_DIFF.exists():
        zf.write(str(GCODE_DIFF), "tools/gcode-diff.exe")
        print("  Added gcode-diff.exe")
    else:
        print("  WARNING: gcode-diff.exe not found")

    # 4. Electron portable exe (if exists)
    if ELECTRON_EXE.exists():
        zf.write(str(ELECTRON_EXE), "tools/" + ELECTRON_EXE.name)
        print(f"  Added Electron portable exe")

elapsed = time.time() - t0
size_mb = OUTPUT.stat().st_size / (1024 * 1024)
print(f"\nDone! {OUTPUT.name}")
print(f"Size: {size_mb:.1f} MB")
print(f"Time: {elapsed:.1f}s")
