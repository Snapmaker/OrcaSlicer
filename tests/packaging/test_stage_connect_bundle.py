import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).parents[2] / "scripts" / "packaging" / "stage_connect_bundle.py"
SPEC = importlib.util.spec_from_file_location("stage_connect_bundle", SCRIPT_PATH)
assert SPEC is not None and SPEC.loader is not None
stage_connect_bundle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stage_connect_bundle)


class StageConnectBundleTest(unittest.TestCase):
    def test_identical_web_fingerprints(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            first = Path(temporary) / "first"
            second = Path(temporary) / "second"
            for root in (first, second):
                (root / "assets").mkdir(parents=True)
                (root / "index.html").write_text("index", encoding="utf-8")
                (root / "assets" / "main.js").write_bytes(b"main")

            self.assertEqual(
                stage_connect_bundle.web_fingerprint(first),
                stage_connect_bundle.web_fingerprint(second),
            )

    def test_changed_web_content_has_different_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            first = Path(temporary) / "first"
            second = Path(temporary) / "second"
            for root in (first, second):
                root.mkdir()
                (root / "index.html").write_text("index", encoding="utf-8")
            (second / "index.html").write_text("changed", encoding="utf-8")

            self.assertNotEqual(
                stage_connect_bundle.web_fingerprint(first),
                stage_connect_bundle.web_fingerprint(second),
            )

    def test_missing_web_directory_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(RuntimeError):
                stage_connect_bundle.web_fingerprint(Path(temporary) / "missing")


if __name__ == "__main__":
    unittest.main()
