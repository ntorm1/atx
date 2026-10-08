"""Isolated operator configuration checks; no service or warehouse mutation."""
from __future__ import annotations

import hashlib
from contextlib import ExitStack
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

_spec = importlib.util.spec_from_file_location("atx_launcher", Path(__file__).with_name(
    "atx-workspace.py"))
assert _spec and _spec.loader
launcher = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(launcher)


class ResearchRegistrationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="atx-launcher-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.manifest = self.root / "atx-db/data/alpha_panel/v1/panel/manifest.json"
        self.manifest.parent.mkdir(parents=True)
        self.content = json.dumps({"schema": "atx.alpha-panel.panel/v2", "status": "complete",
                                   "files": {}}).encode()
        self.manifest.write_bytes(self.content)
        self.sha = hashlib.sha256(self.content).hexdigest()

    def test_autodetection_pins_exact_bytes_without_mutating_manifest(self) -> None:
        registered = launcher.research_configuration(self.root, [], False, {})
        self.assertEqual(registered, [f"atx-equity-panel={self.sha}={self.manifest.resolve()}"])
        self.assertEqual(self.manifest.read_bytes(), self.content)

    def test_explicit_revision_refuses_changed_manifest(self) -> None:
        registration = f"equities={self.sha}={self.manifest}"
        self.manifest.write_bytes(self.content + b" ")
        with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
            launcher.research_configuration(self.root, [registration], False, {})

    def test_explicit_registration_overrides_autodetection(self) -> None:
        registered = launcher.research_configuration(self.root,
            [f"train={self.sha.upper()}={self.manifest}"], False, {})
        self.assertEqual(len(registered), 1)
        self.assertTrue(registered[0].startswith(f"train={self.sha}="))

    def test_incomplete_and_oversized_receipts_are_refused(self) -> None:
        self.manifest.write_text('{"schema":"atx.alpha-panel.panel/v2","status":"building"}')
        with self.assertRaisesRegex(ValueError, "complete"):
            launcher.research_configuration(self.root, [], False, {})
        self.manifest.write_bytes(b" " * (1024 * 1024 + 1))
        with self.assertRaisesRegex(ValueError, "1 MiB"):
            launcher.research_configuration(self.root, [], False, {})

    def test_duplicate_ids_and_excessive_registrations_are_refused(self) -> None:
        source = f"train={self.manifest}"
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            launcher.research_configuration(self.root, [source, source], False, {})
        with self.assertRaisesRegex(ValueError, "32"):
            launcher.research_configuration(self.root, [source] * 33, False, {})

    def test_opt_out_is_explicit_and_no_missing_dataset_is_fabricated(self) -> None:
        self.assertEqual(launcher.research_configuration(self.root, [], True, {}), [])
        with self.assertRaisesRegex(ValueError, "cannot be combined"):
            launcher.research_configuration(self.root, [f"train={self.manifest}"], True, {})
        self.manifest.unlink()
        self.assertEqual(launcher.research_configuration(self.root, [], False, {}), [])

    def test_native_snapshot_preserves_build_outputs_and_owns_its_cleanup(self) -> None:
        build = self.root / "build"
        runtime = self.root / "runtime"
        build.mkdir()
        runtime.mkdir()
        binary = build / "server.exe"
        binary.write_bytes(b"first build")
        (build / "duckdb.dll").write_bytes(b"dependency")
        (build / "atx-server-THIRD_PARTY_NOTICES.txt").write_text("notice")
        with ExitStack() as stack:
            launched = launcher.snapshot_native(stack, binary, runtime)
            self.assertEqual(launched.parent.parent, runtime)
            binary.write_bytes(b"next build")
            self.assertEqual(launched.read_bytes(), b"first build")
            self.assertEqual((launched.parent / "duckdb.dll").read_bytes(), b"dependency")
            self.assertTrue((launched.parent / "atx-server-THIRD_PARTY_NOTICES.txt").is_file())
        self.assertFalse(launched.parent.exists())
        self.assertEqual(binary.read_bytes(), b"next build")


if __name__ == "__main__":
    unittest.main()
