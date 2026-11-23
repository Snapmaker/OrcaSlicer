import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).parents[2] / "scripts" / "packaging" / "create_dmg.py"
SPEC = importlib.util.spec_from_file_location("create_dmg", SCRIPT)
create_dmg = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(create_dmg)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class DmgCapacityTest(unittest.TestCase):
    def test_sparse_file_uses_logical_size(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            with (source / "sparse executable").open("wb") as stream:
                stream.seek(128 * create_dmg.MIB - 1)
                stream.write(b"\0")
            self.assertGreater(create_dmg.image_size_mib(source), 128 + 64)

    @unittest.skipUnless(sys.platform == "darwin", "requires macOS hdiutil")
    def test_dmg_round_trip(self):
        with tempfile.TemporaryDirectory(prefix="dmg regression ") as directory:
            root = Path(directory)
            source = root / "source folder"
            binary = source / "Snapmaker_Orca_profile_validator.app/Contents/MacOS/Snapmaker_Orca_profile_validator"
            binary.parent.mkdir(parents=True)
            # A small allocated footprint with a large logical payload catches
            # capacity estimates based only on occupied host-filesystem blocks.
            with binary.open("wb") as stream:
                stream.write(b"validator payload\n")
                stream.seek(128 * create_dmg.MIB - 16)
                stream.write(b"end of payload\n")
            binary.chmod(0o755)
            (source / "Applications").symlink_to("/Applications")
            # Do not traverse the Applications symlink during size estimation.
            self.assertLess(create_dmg.image_size_mib(source), 256)
            image = root / "validator image.dmg"
            subprocess.run([
                sys.executable, str(SCRIPT), "--source", str(source),
                "--output", str(image), "--volume-name", "Snapmaker_Orca Profile Validator",
            ], check=True)
            subprocess.run(["hdiutil", "verify", str(image)], check=True)
            mount = root / "mounted image"
            mount.mkdir()
            subprocess.run([
                "hdiutil", "attach", str(image), "-readonly", "-nobrowse",
                "-mountpoint", str(mount),
            ], check=True)
            try:
                copied = mount / binary.relative_to(source)
                self.assertEqual(digest(binary), digest(copied))
                self.assertTrue(os.access(copied, os.X_OK))
                self.assertEqual(os.readlink(mount / "Applications"), "/Applications")
            finally:
                subprocess.run(["hdiutil", "detach", str(mount)], check=True)


if __name__ == "__main__":
    unittest.main()
