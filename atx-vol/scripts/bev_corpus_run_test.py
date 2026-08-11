"""Tests for bev_corpus_run.py (atx-vol Task 4: corpus batch runner).

Pure stdlib (unittest), self-contained tmp-dir fixtures, no network, no real
corpus/driver dependency -- the "live run" tests below drive a tiny Python
stub (registered via --exe) instead of the real bev_label_factory binary.

Run: python -m pytest atx-vol/scripts/bev_corpus_run_test.py -q
"""

from __future__ import annotations

import contextlib
import io
import json
import unittest
from pathlib import Path

from bev_corpus_run import ManifestError, build_invocations, load_manifest, main

# A stub driver standing in for bev_label_factory. It validates it received
# --out, then either fails (uid == "FAIL") or writes a minimal meta-header +
# header-only TSV and exits 0. Invoked as `sys.executable stub.py ...` so the
# test stays subprocess-friendly / platform-clean on Windows.
_STUB_SOURCE = """
import argparse
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--db", required=True)
parser.add_argument("--uid", required=True)
parser.add_argument("--entry-start", required=True)
parser.add_argument("--entry-end", required=True)
parser.add_argument("--tenor-days", required=True)
parser.add_argument("--delta-lo", required=True)
parser.add_argument("--delta-hi", required=True)
parser.add_argument("--dividends", required=True)
parser.add_argument("--events", default="")
parser.add_argument("--out", required=True)
parser.add_argument("--threads", required=True)
args = parser.parse_args()

if args.uid == "FAIL":
    sys.stderr.write("stub: forced failure for uid=FAIL\\n")
    sys.exit(1)

with open(args.out, "w", encoding="utf-8") as f:
    f.write("# tool=stub\\n")
    f.write("# n_rows_written=3\\n")
    f.write("entry_ts_ns\\tuid\\tstrike\\n")
    f.write("0\\tSTUB\\t1.0\\n")
sys.exit(0)
"""


def _write_manifest(tmp_path: Path, uid0: str = "SPY", uid1: str = "QQQ") -> tuple[Path, dict]:
    manifest = {
        "defaults": {"delta_lo": 0.05, "delta_hi": 0.95, "threads": 0},
        "tenor_days": [30, 60],
        "runs": [
            {
                "db": "C:/atx-data/surface-db-r2/spy-2019",
                "uid": uid0,
                "entry_start": "2019-01-02",
                "entry_end": "2019-12-31",
                "dividends": "C:/atx-data/div/spy.tsv",
                "events": "",
            },
            {
                "db": "C:/atx-data/surface-db-r2/qqq-2020",
                "uid": uid1,
                "entry_start": "2020-01-02",
                "entry_end": "2020-12-31",
                "dividends": "C:/atx-data/div/qqq.tsv",
                "events": "C:/atx-data/events/qqq.tsv",
            },
        ],
    }
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path, manifest


def _write_stub(tmp_path: Path) -> Path:
    stub_path = tmp_path / "stub_driver.py"
    stub_path.write_text(_STUB_SOURCE, encoding="utf-8")
    return stub_path


def _run_main(args: list[str]) -> tuple[int, str]:
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = main(args)
    return rc, buf.getvalue()


class DryRunTest(unittest.TestCase):
    def test_prints_four_exact_argv_lists_for_2x2_manifest(self) -> None:
        tmp = Path(self._tmp_dir())
        manifest_path, manifest = _write_manifest(tmp)
        out_dir = tmp / "out"
        exe = "bev_label_factory.exe"

        rc, printed = _run_main(
            ["--manifest", str(manifest_path), "--exe", exe, "--out-dir", str(out_dir), "--dry-run"]
        )

        self.assertEqual(rc, 0)
        lines = [line for line in printed.splitlines() if line.strip()]
        self.assertEqual(len(lines), 4)
        argvs = [json.loads(line) for line in lines]

        defaults = manifest["defaults"]
        run0, run1 = manifest["runs"]

        def expected(run: dict, tenor: int, events: str | None) -> list[str]:
            out_path = out_dir / f"{run['uid']}_{run['entry_start']}_{tenor}d.tsv"
            cmd = [
                exe,
                "--db", run["db"],
                "--uid", run["uid"],
                "--entry-start", run["entry_start"],
                "--entry-end", run["entry_end"],
                "--tenor-days", str(tenor),
                "--delta-lo", str(defaults["delta_lo"]),
                "--delta-hi", str(defaults["delta_hi"]),
                "--dividends", run["dividends"],
            ]
            if events:
                cmd += ["--events", events]
            cmd += ["--out", str(out_path)]
            cmd += ["--threads", str(defaults["threads"])]
            return cmd

        self.assertEqual(argvs[0], expected(run0, 30, None))
        self.assertEqual(argvs[1], expected(run0, 60, None))
        self.assertEqual(argvs[2], expected(run1, 30, run1["events"]))
        self.assertEqual(argvs[3], expected(run1, 60, run1["events"]))

        # Dry-run must not execute anything or touch the filesystem beyond
        # what argparse/pathlib construction itself needs.
        self.assertFalse(out_dir.exists())

    # -- tmp_path-equivalent helper (unittest has no tmp_path fixture) --
    def setUp(self) -> None:
        import tempfile

        self._tmpdir_obj = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self._tmpdir_obj.cleanup()

    def _tmp_dir(self) -> str:
        return self._tmpdir_obj.name


class LiveRunTest(unittest.TestCase):
    def setUp(self) -> None:
        import tempfile

        self._tmpdir_obj = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmpdir_obj.name)

    def tearDown(self) -> None:
        self._tmpdir_obj.cleanup()

    def test_live_run_produces_four_outputs_and_manifest_out_with_row_counts(self) -> None:
        manifest_path, manifest = _write_manifest(self.tmp)
        stub_path = _write_stub(self.tmp)
        out_dir = self.tmp / "out"

        rc, _ = _run_main(
            ["--manifest", str(manifest_path), "--exe", str(stub_path), "--out-dir", str(out_dir)]
        )

        self.assertEqual(rc, 0)

        expected_names = [
            "SPY_2019-01-02_30d.tsv",
            "SPY_2019-01-02_60d.tsv",
            "QQQ_2020-01-02_30d.tsv",
            "QQQ_2020-01-02_60d.tsv",
        ]
        for name in expected_names:
            self.assertTrue((out_dir / name).exists(), f"missing output {name}")
            log_name = name.replace(".tsv", ".log")
            self.assertTrue((out_dir / log_name).exists(), f"missing log {log_name}")

        manifest_out_path = out_dir / "manifest_out.json"
        self.assertTrue(manifest_out_path.exists())
        manifest_out = json.loads(manifest_out_path.read_text(encoding="utf-8"))

        self.assertTrue(manifest_out["ok"])
        self.assertEqual(len(manifest_out["invocations"]), 4)
        for inv in manifest_out["invocations"]:
            self.assertEqual(inv["returncode"], 0)
            self.assertEqual(inv["meta"]["n_rows_written"], "3")

    def test_one_failing_combination_yields_nonzero_exit_and_recorded_failure(self) -> None:
        # uid "FAIL" makes the stub exit 1 for both of that run's tenors.
        manifest_path, manifest = _write_manifest(self.tmp, uid1="FAIL")
        stub_path = _write_stub(self.tmp)
        out_dir = self.tmp / "out"

        rc, _ = _run_main(
            ["--manifest", str(manifest_path), "--exe", str(stub_path), "--out-dir", str(out_dir)]
        )

        self.assertNotEqual(rc, 0)

        manifest_out = json.loads((out_dir / "manifest_out.json").read_text(encoding="utf-8"))
        self.assertFalse(manifest_out["ok"])

        by_uid: dict[str, list[dict]] = {}
        for inv in manifest_out["invocations"]:
            by_uid.setdefault(inv["uid"], []).append(inv)

        # The failing run's two invocations are recorded with a nonzero
        # returncode and no parsed meta (the stub never wrote a TSV).
        self.assertEqual(len(by_uid["FAIL"]), 2)
        for inv in by_uid["FAIL"]:
            self.assertNotEqual(inv["returncode"], 0)
            self.assertEqual(inv["meta"], {})
            self.assertFalse(Path(inv["out"]).exists())

        # The other run's two combinations still ran successfully despite
        # the failure elsewhere in the manifest.
        self.assertEqual(len(by_uid["SPY"]), 2)
        for inv in by_uid["SPY"]:
            self.assertEqual(inv["returncode"], 0)
            self.assertEqual(inv["meta"]["n_rows_written"], "3")
            self.assertTrue(Path(inv["out"]).exists())

    def test_nonexistent_exe_records_failure_and_still_writes_manifest_out(self) -> None:
        # --exe pointing at a path that doesn't exist makes subprocess.run
        # raise OSError (FileNotFoundError on Windows/POSIX) rather than
        # returning a completed process -- this must not abort the batch
        # before manifest_out.json is written.
        manifest_path, manifest = _write_manifest(self.tmp)
        out_dir = self.tmp / "out"
        missing_exe = str(self.tmp / "does-not-exist.exe")

        rc, _ = _run_main(
            ["--manifest", str(manifest_path), "--exe", missing_exe, "--out-dir", str(out_dir)]
        )

        self.assertNotEqual(rc, 0)

        manifest_out_path = out_dir / "manifest_out.json"
        self.assertTrue(manifest_out_path.exists())
        manifest_out = json.loads(manifest_out_path.read_text(encoding="utf-8"))
        self.assertFalse(manifest_out["ok"])
        self.assertEqual(len(manifest_out["invocations"]), 4)
        for inv in manifest_out["invocations"]:
            self.assertNotEqual(inv["returncode"], 0)
            self.assertEqual(inv["meta"], {})


class ManifestValidationTest(unittest.TestCase):
    def setUp(self) -> None:
        import tempfile

        self._tmpdir_obj = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmpdir_obj.name)

    def tearDown(self) -> None:
        self._tmpdir_obj.cleanup()

    def test_missing_top_level_key_names_the_field(self) -> None:
        path = self.tmp / "manifest.json"
        path.write_text(json.dumps({"defaults": {"delta_lo": 0.05, "delta_hi": 0.95}, "runs": []}), encoding="utf-8")

        with self.assertRaises(ManifestError) as ctx:
            load_manifest(path)
        self.assertIn("tenor_days", str(ctx.exception))

    def test_missing_run_key_names_the_field_and_index(self) -> None:
        manifest = {
            "defaults": {"delta_lo": 0.05, "delta_hi": 0.95},
            "tenor_days": [30],
            "runs": [{"uid": "SPY", "entry_start": "2019-01-02", "entry_end": "2019-12-31", "dividends": "d.tsv"}],
        }
        path = self.tmp / "manifest.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaises(ManifestError) as ctx:
            load_manifest(path)
        self.assertIn("db", str(ctx.exception))
        self.assertIn("runs[0]", str(ctx.exception))

    def test_invalid_json_raises_manifest_error_naming_file(self) -> None:
        path = self.tmp / "manifest.json"
        path.write_text("{not valid json", encoding="utf-8")

        with self.assertRaises(ManifestError) as ctx:
            load_manifest(path)
        self.assertIn(str(path), str(ctx.exception))

    def test_missing_manifest_file_raises_manifest_error_naming_file(self) -> None:
        path = self.tmp / "does-not-exist.json"

        with self.assertRaises(ManifestError) as ctx:
            load_manifest(path)
        self.assertIn(str(path), str(ctx.exception))

    def test_main_exits_with_config_error_code_on_invalid_json(self) -> None:
        path = self.tmp / "manifest.json"
        path.write_text("{not valid json", encoding="utf-8")
        out_dir = self.tmp / "out"

        rc, _ = _run_main(
            ["--manifest", str(path), "--exe", "bev_label_factory.exe", "--out-dir", str(out_dir)]
        )
        self.assertEqual(rc, 2)

    def test_main_exits_with_config_error_code_on_missing_manifest_file(self) -> None:
        path = self.tmp / "does-not-exist.json"
        out_dir = self.tmp / "out"

        rc, _ = _run_main(
            ["--manifest", str(path), "--exe", "bev_label_factory.exe", "--out-dir", str(out_dir)]
        )
        self.assertEqual(rc, 2)


def _collision_manifest() -> dict:
    # Two runs sharing (uid, entry_start, tenor_days) but differing in db
    # and entry_end -- both resolve to the same out_path
    # "SPY_2019-01-02_30d.tsv" and would silently overwrite each other.
    return {
        "defaults": {"delta_lo": 0.05, "delta_hi": 0.95, "threads": 0},
        "tenor_days": [30],
        "runs": [
            {
                "db": "C:/atx-data/surface-db-r2/spy-2019",
                "uid": "SPY",
                "entry_start": "2019-01-02",
                "entry_end": "2019-12-31",
                "dividends": "C:/atx-data/div/spy.tsv",
                "events": "",
            },
            {
                "db": "C:/atx-data/surface-db-r2/spy-2019-restated",
                "uid": "SPY",
                "entry_start": "2019-01-02",
                "entry_end": "2019-06-30",
                "dividends": "C:/atx-data/div/spy.tsv",
                "events": "",
            },
        ],
    }


class OutputCollisionTest(unittest.TestCase):
    def setUp(self) -> None:
        import tempfile

        self._tmpdir_obj = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmpdir_obj.name)

    def tearDown(self) -> None:
        self._tmpdir_obj.cleanup()

    def test_build_invocations_raises_naming_the_colliding_uid_entry_start_tenor(self) -> None:
        manifest = _collision_manifest()

        with self.assertRaises(ManifestError) as ctx:
            build_invocations(manifest, "bev_label_factory.exe", self.tmp / "out")

        msg = str(ctx.exception)
        self.assertIn("SPY", msg)
        self.assertIn("2019-01-02", msg)
        self.assertIn("30", msg)

    def test_main_full_run_rejects_collision_before_any_launch_or_write(self) -> None:
        manifest_path = self.tmp / "manifest.json"
        manifest_path.write_text(json.dumps(_collision_manifest()), encoding="utf-8")
        stub_path = _write_stub(self.tmp)
        out_dir = self.tmp / "out"

        rc, _ = _run_main(
            ["--manifest", str(manifest_path), "--exe", str(stub_path), "--out-dir", str(out_dir)]
        )

        self.assertNotEqual(rc, 0)
        # Neither the stub subprocess ran nor was out_dir (or anything in
        # it, including manifest_out.json) ever created.
        self.assertFalse(out_dir.exists())

    def test_main_dry_run_rejects_collision_before_printing_any_argv(self) -> None:
        manifest_path = self.tmp / "manifest.json"
        manifest_path.write_text(json.dumps(_collision_manifest()), encoding="utf-8")
        out_dir = self.tmp / "out"

        rc, printed = _run_main(
            [
                "--manifest", str(manifest_path),
                "--exe", "bev_label_factory.exe",
                "--out-dir", str(out_dir),
                "--dry-run",
            ]
        )

        self.assertNotEqual(rc, 0)
        self.assertEqual(printed, "")
        self.assertFalse(out_dir.exists())


if __name__ == "__main__":
    unittest.main()
