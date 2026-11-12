#!/usr/bin/env python3
"""CLI/OpenGL regression test; requires a built app and a working OpenGL context.

Run: python tests/cli/test_thumbnail_export.py --executable <snapmaker-orca executable>
Uses isolated data directories and keeps logs, 3MFs, and PNGs in --output-dir.
Only Python's standard library is required. This is separate from headless CTest.
"""

import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile
import zlib


def check_thumbnail(png):
    """Decode the exporter's RGBA PNG and reject empty or full-frame output."""
    assert png[:8] == b"\x89PNG\r\n\x1a\n", "Invalid PNG signature"
    compressed = bytearray()
    offset = 8
    dimensions = None
    while offset < len(png):
        size = struct.unpack_from(">I", png, offset)[0]
        kind = png[offset + 4:offset + 8]
        data = png[offset + 8:offset + 8 + size]
        crc = struct.unpack_from(">I", png, offset + 8 + size)[0]
        assert zlib.crc32(kind + data) == crc, "Invalid PNG chunk CRC"
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", data)
            assert (depth, color, compression, filtering, interlace) == (8, 6, 0, 0, 0)
            dimensions = width, height
        elif kind == b"IDAT":
            compressed.extend(data)
        offset += size + 12
    assert dimensions == (512, 512), f"Unexpected dimensions: {dimensions}"
    stride = width * 4
    rows = zlib.decompress(compressed)
    assert len(rows) == height * (stride + 1), "Invalid PNG pixel data"
    previous = bytearray(stride)
    visible = 0
    for offset in range(0, len(rows), stride + 1):
        filter_type = rows[offset]
        assert filter_type <= 4, "Invalid PNG row filter"
        row = bytearray(rows[offset + 1:offset + 1 + stride])
        for i in range(stride):
            left = row[i - 4] if i >= 4 else 0
            up = previous[i]
            upper_left = previous[i - 4] if i >= 4 else 0
            if filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = up
            elif filter_type == 3:
                predictor = (left + up) // 2
            elif filter_type == 4:
                p = left + up - upper_left
                predictor = min((left, up, upper_left), key=lambda value: abs(p - value))
            else:
                predictor = 0
            row[i] = (row[i] + predictor) & 255
        visible += sum(alpha > 0 for alpha in row[3::4])
        previous = row
    assert 0 < visible < width * height, f"No model silhouette: {visible} visible pixels"
    return {"width": width, "height": height, "visible_pixels": visible}


def export_and_check(executable, model, directory):
    directory.mkdir(parents=True, exist_ok=False)
    command = [
        str(executable), "--debug", "3", "--arrange", "1",
        "--export-3mf", "result.3mf", "--outputdir", str(directory),
        "--datadir", str(directory / "data"), str(model),
    ]
    (directory / "command.json").write_text(json.dumps(command, indent=2), encoding="utf-8")
    with (directory / "app.log").open("wb") as log:
        process = subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    assert process.returncode == 0, f"CLI exited with {process.returncode}; see {directory / 'app.log'}"
    result = directory / "result.3mf"
    thumbnails = {}
    with zipfile.ZipFile(result) as archive:
        for name in ("plate_1.png", "plate_no_light_1.png", "top_1.png", "pick_1.png"):
            png = archive.read("Metadata/" + name)
            (directory / name).write_bytes(png)
            thumbnails[name] = check_thumbnail(png)
    return result, {"exit_code": process.returncode, "thumbnails": thumbnails}


def make_painted_fixture(source, destination):
    """Exercise mmuseg_models, forcing regeneration instead of reusing cached PNGs."""
    triangles = 0
    with zipfile.ZipFile(source) as original, zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as painted:
        for entry in original.infolist():
            if entry.filename.lower().endswith(".png"):
                continue
            data = original.read(entry)
            if entry.filename == "Metadata/project_settings.config":
                # OBJ export has no preset IDs; the project-import CLI expects these fields.
                settings = json.loads(data)
                settings.setdefault("printer_settings_id", "CLI thumbnail test printer")
                settings.setdefault("print_settings_id", "CLI thumbnail test process")
                settings.setdefault("filament_settings_id", ["CLI thumbnail test filament"])
                data = json.dumps(settings).encode("utf-8")
            if entry.filename.endswith(".model"):
                # Unsplit triangle, extruder 1: two split bits 00 and two state bits 01.
                data, count = re.subn(rb"(<triangle\b[^>]*?)(\s*/>)", rb'\1 paint_color="4"\2', data)
                triangles += count
            painted.writestr(entry, data)
    assert triangles > 0, "Fixture contains no paintable triangles"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    output = (args.output_dir or Path(tempfile.mkdtemp(prefix="cli-thumbnail-regression-"))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    print(f"Evidence: {output}", flush=True)
    cube = Path(__file__).resolve().parents[1] / "data" / "20mm_cube.obj"
    plain, plain_report = export_and_check(executable, cube, output / "plain")
    painted = output / "painted.3mf"
    make_painted_fixture(plain, painted)
    painted_result, painted_report = export_and_check(executable, painted, output / "painted")
    with zipfile.ZipFile(painted_result) as archive:
        assert any(b'paint_color="4"' in archive.read(name) for name in archive.namelist() if name.endswith(".model")), \
            "Paint annotations were lost; the segmented-model path was not exercised"
    report = {"plain": plain_report, "painted": painted_report}
    (output / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
