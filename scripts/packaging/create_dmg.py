#!/usr/bin/env python3
"""Create a compressed HFS+ DMG with room for the source's logical contents."""

import argparse
import os
from pathlib import Path
import subprocess


MIB = 1024 * 1024


def image_size_mib(source: Path) -> int:
    # Allocated blocks can undercount sparse/compressed files (including lipo
    # output). Count logical bytes and round each entry to an HFS+ block instead.
    size = 4096
    for root, directories, files in os.walk(source, followlinks=False):
        for name in directories + files:
            entry_size = (Path(root) / name).lstat().st_size
            size += max(4096, ((entry_size + 4095) // 4096) * 4096)
    # Leave 20% plus 64 MiB for filesystem metadata and copy-time overhead.
    return (size * 6 // 5 + 64 * MIB + MIB - 1) // MIB


def create_dmg(source: Path, output: Path, volume_name: str) -> None:
    if not source.is_dir():
        raise ValueError(f"DMG source directory does not exist: {source}")
    if source.resolve() in output.resolve().parents:
        raise ValueError("DMG output must be outside its source directory")
    size = image_size_mib(source)
    print(f"Creating {output} from {source} with {size} MiB capacity", flush=True)
    subprocess.run([
        "hdiutil", "create", "-volname", volume_name, "-srcfolder", str(source),
        "-fs", "HFS+", "-size", f"{size}m", "-ov", "-format", "UDZO", str(output),
    ], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--volume-name", required=True)
    args = parser.parse_args()
    create_dmg(args.source, args.output, args.volume_name)
