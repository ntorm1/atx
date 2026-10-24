"""Explicit collector CLI controls; numerical completeness has a separate reader."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.environ.get("ATX_QUOTE_BENCH_BINARY"), "native binary not configured")
class CaptureCli(unittest.TestCase):
    def test_existing_capture_and_foreign_worker_scope_are_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = root / "capture.json"
            capture.write_text("preserve me", encoding="utf-8")
            for workers in ("1", "2"):
                result = subprocess.run([
                    os.environ["ATX_QUOTE_BENCH_BINARY"], "--input", str(root / "missing.csv"),
                    "--output", str(root / "predictions.csv"), "--report", str(root / "report.json"),
                    "--candidate-capture", str(capture), "--timing-detail", "--cold-reset",
                ], env=dict(os.environ, ATX_VOL_FIT_WORKERS=workers),
                    capture_output=True, text=True, timeout=30, check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("candidate capture requires", result.stderr)
                self.assertEqual(capture.read_text(encoding="utf-8"), "preserve me")
                self.assertEqual(list(root.iterdir()), [capture])

    def test_refuses_non_single_scope_before_reading_or_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for extra in (["--reps", "2"], ["--acceptance-audit"], ["--curve", "essvi"]):
                with self.subTest(extra=extra):
                    env = dict(os.environ, ATX_VOL_FIT_WORKERS="1")
                    result = subprocess.run([
                        os.environ["ATX_QUOTE_BENCH_BINARY"], "--input", str(root / "missing.csv"),
                        "--output", str(root / "predictions.csv"), "--report", str(root / "report.json"),
                        "--candidate-capture", str(root / "capture.json"),
                        "--timing-detail", "--cold-reset", *extra,
                    ], env=env, capture_output=True, text=True, timeout=30, check=False)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("candidate capture requires", result.stderr)
                    self.assertEqual(list(root.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
