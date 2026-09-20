#!/usr/bin/env python3
"""Download, hash-check, and stage the snapmaker_connect release bundle."""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import stat
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path


RELEASE_REPOSITORY = "Snapmaker/snapmaker-flutter-web"
ASSET_KEYS = {
    "windows-x64": ("zip", "snapmaker_connection_windows_x64.exe"),
    "windows-arm64": ("zip", "snapmaker_connection_windows_arm64.exe"),
    "macos-x64": ("zip", "snapmaker_connection_macos_x64"),
    "macos-arm64": ("zip", "snapmaker_connection_macos_arm64"),
    "linux-x64": ("tar.gz", "snapmaker_connection_linux_x64"),
    "linux-arm64": ("tar.gz", "snapmaker_connection_linux_arm64"),
}
STAGE_PLATFORMS = {
    "windows-x64": ("windows-x64",),
    "windows-arm64": ("windows-arm64",),
    "macos-universal": ("macos-x64", "macos-arm64"),
    "linux-x64": ("linux-x64",),
    "linux-arm64": ("linux-arm64",),
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def web_fingerprint(root: Path) -> dict[str, str]:
    if not root.is_dir():
        raise RuntimeError(f"flutter_web directory does not exist: {root}")
    return {
        path.relative_to(root).as_posix(): sha256_file(path)
        for path in root.rglob("*")
        if path.is_file()
    }


def default_cache_dir() -> Path:
    if os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
        return base / "OrcaSlicer" / "connect-bundle-cache"
    base = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    return base / "OrcaSlicer" / "connect-bundles"


def load_lock(path: Path, asset_keys: tuple[str, ...]) -> tuple[str, dict[str, str]]:
    import json

    try:
        lock = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RuntimeError(f"cannot read lock file {path}: {error}") from error

    tag = lock.get("releaseTag") if isinstance(lock, dict) else None
    hashes = lock.get("sha256") if isinstance(lock, dict) else None
    if not isinstance(tag, str) or not tag.startswith("v") or len(tag) == 1:
        raise RuntimeError("lock file releaseTag must be a release tag such as v1.3.0")
    if not isinstance(hashes, dict):
        raise RuntimeError("lock file must contain a sha256 object")

    selected = {}
    for asset_key in asset_keys:
        value = hashes.get(asset_key)
        if not isinstance(value, str) or len(value) != 64 or any(c not in "0123456789abcdefABCDEF" for c in value):
            raise RuntimeError(f"lock file has no valid SHA256 for {asset_key}")
        selected[asset_key] = value.lower()
    return tag, selected


def asset_filename(tag: str, asset_key: str) -> str:
    archive_format, _ = ASSET_KEYS[asset_key]
    return f"snapmaker_connection-{tag[1:]}-{asset_key}.{archive_format}"


def download_url(tag: str, filename: str) -> str:
    quoted_filename = urllib.parse.quote(filename)
    return f"https://github.com/{RELEASE_REPOSITORY}/releases/download/{urllib.parse.quote(tag)}/{quoted_filename}"


def prepare_archive(cache_dir: Path, tag: str, asset_key: str, expected_hash: str) -> Path:
    filename = asset_filename(tag, asset_key)
    archive_path = cache_dir / filename
    if archive_path.is_file() and sha256_file(archive_path) == expected_hash:
        print(f"connect bundle cache hit: {archive_path}")
        return archive_path

    if archive_path.exists():
        print(f"connect bundle cache mismatch, downloading again: {archive_path}")

    partial_path = cache_dir / f".{filename}.part"
    partial_path.unlink(missing_ok=True)
    request = urllib.request.Request(download_url(tag, filename), headers={"User-Agent": "OrcaSlicer-packaging"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response, partial_path.open("wb") as target:
            shutil.copyfileobj(response, target, 1024 * 1024)
    except Exception as error:
        partial_path.unlink(missing_ok=True)
        raise RuntimeError(f"cannot download {filename}; expected manual archive path: {archive_path}") from error

    actual_hash = sha256_file(partial_path)
    if actual_hash != expected_hash:
        partial_path.unlink(missing_ok=True)
        raise RuntimeError(f"SHA256 mismatch for {filename}: expected {expected_hash}, got {actual_hash}")
    archive_path.unlink(missing_ok=True)
    partial_path.replace(archive_path)
    return archive_path


def ensure_inside(root: Path, target: Path) -> None:
    try:
        target.resolve(strict=False).relative_to(root.resolve(strict=False))
    except ValueError as error:
        raise RuntimeError(f"archive entry escapes the destination: {target}") from error


def extract_archive(archive_path: Path, destination: Path, asset_key: str) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    if asset_key.startswith("linux-"):
        with tarfile.open(archive_path, "r:gz") as archive:
            for member in archive:
                if member.issym() or member.islnk() or member.isdev():
                    raise RuntimeError(f"unsupported archive entry type: {member.name}")
                target = destination / member.name
                ensure_inside(destination, target)
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                elif member.isreg():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    source = archive.extractfile(member)
                    if source is None:
                        raise RuntimeError(f"cannot extract archive entry: {member.name}")
                    with source, target.open("wb") as output:
                        shutil.copyfileobj(source, output, 1024 * 1024)
                    os.chmod(target, member.mode & 0o777 or 0o644)
                else:
                    raise RuntimeError(f"unsupported archive entry type: {member.name}")
        return

    with zipfile.ZipFile(archive_path, "r") as archive:
        for info in archive.infolist():
            mode = info.external_attr >> 16
            if stat.S_ISLNK(mode):
                raise RuntimeError(f"unsupported archive entry type: {info.filename}")
            target = destination / info.filename
            ensure_inside(destination, target)
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, target.open("wb") as output:
                shutil.copyfileobj(source, output, 1024 * 1024)
            os.chmod(target, mode & 0o777 or 0o644)


def remove_stale_cli_files(resources_dir: Path, selected_keys: tuple[str, ...]) -> None:
    expected = {ASSET_KEYS[key][1] for key in selected_keys}
    for filename in set(ASSET_KEYS.values()):
        cli_name = filename[1]
        if cli_name in expected:
            continue
        path = resources_dir / cli_name
        if path.is_symlink() or path.is_file():
            path.unlink()


def replace_directory(source: Path, target: Path, resources_dir: Path) -> None:
    ensure_inside(resources_dir, target)
    if target.is_symlink() or target.is_file():
        target.unlink()
    elif target.exists():
        shutil.rmtree(target)
    shutil.copytree(source, target)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", default=".github/connect-bundle.lock.json", help="connect bundle lock file")
    parser.add_argument("--platform", required=True, choices=sorted(STAGE_PLATFORMS), help="target staging platform")
    parser.add_argument("--resources-dir", default="resources", help="Orca resources directory")
    parser.add_argument("--cache-dir", type=Path, help="archive cache directory")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    asset_keys = STAGE_PLATFORMS[args.platform]
    lock_path = Path(args.lock)
    resources_dir = Path(args.resources_dir)
    cache_dir = args.cache_dir or default_cache_dir()
    tag, hashes = load_lock(lock_path, asset_keys)
    cache_dir.mkdir(parents=True, exist_ok=True)
    resources_dir.mkdir(parents=True, exist_ok=True)

    print(f"Staging snapmaker_connect {tag} for {args.platform}")
    print(f"Connect bundle cache: {cache_dir}")
    with tempfile.TemporaryDirectory(prefix=".connect-stage-", dir=cache_dir) as temporary:
        extracted_root = Path(temporary)
        web_sources: list[Path] = []
        for asset_key in asset_keys:
            archive_path = prepare_archive(cache_dir, tag, asset_key, hashes[asset_key])
            extract_destination = extracted_root / asset_key
            extract_archive(archive_path, extract_destination, asset_key)
            cli_source = extract_destination / ASSET_KEYS[asset_key][1]
            if not cli_source.is_file():
                raise RuntimeError(f"archive {archive_path.name} does not contain {ASSET_KEYS[asset_key][1]}")
            shutil.copy2(cli_source, resources_dir / ASSET_KEYS[asset_key][1])
            os.chmod(resources_dir / ASSET_KEYS[asset_key][1], 0o755)
            web_source = extract_destination / "web" / "flutter_web"
            if not web_source.is_dir():
                raise RuntimeError(f"archive {archive_path.name} does not contain web/flutter_web")
            web_sources.append(web_source)

        web_fingerprints = [web_fingerprint(web_source) for web_source in web_sources]
        if any(fingerprint != web_fingerprints[0] for fingerprint in web_fingerprints[1:]):
            raise RuntimeError(f"flutter_web contents differ between archives for {args.platform}")

        remove_stale_cli_files(resources_dir, asset_keys)
        replace_directory(web_sources[0], resources_dir / "web" / "flutter_web", resources_dir)

    print(f"Staged connect CLI and flutter_web into {resources_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
