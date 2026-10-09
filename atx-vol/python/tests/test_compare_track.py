"""Contract tests for tools/compare_track.py.

The comparator is deliberately standalone: this module uses only the standard
library and can be run directly when the atxvol extension is unavailable.  It
is also unittest-shaped so the registered pytest lane collects the same cases.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TOOL = Path(__file__).resolve().parents[2] / "tools" / "compare_track.py"


class CompareTrackTest(unittest.TestCase):
    def compare(self, golden: str, candidate: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory(prefix="atx_compare_track_") as raw:
            root = Path(raw)
            golden_path = root / "golden.tsv"
            candidate_path = root / "candidate.tsv"
            golden_path.write_text(golden, encoding="utf-8", newline="")
            candidate_path.write_text(candidate, encoding="utf-8", newline="")
            return subprocess.run(
                [sys.executable, str(TOOL), str(golden_path), str(candidate_path)],
                capture_output=True,
                text=True,
                check=False,
            )

    @staticmethod
    def track(*rows: str, header: str = "date\tn_open_lots\tpnl_total") -> str:
        return "# strategy=fixture\n" + header + "\n" + "\n".join(rows) + "\n"

    def test_byte_equal_reports_byte_pass(self) -> None:
        track = self.track("2026-01-02\t1\t1.0", "2026-01-05\t2\t-2.0")
        proc = self.compare(track, track)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.strip(), "PASS byte")

    def test_small_float_drift_reports_parity_and_per_column_drift(self) -> None:
        golden = self.track("2026-01-02\t1\t1.0")
        candidate = self.track("2026-01-02\t1\t1.000000000001")
        proc = self.compare(golden, candidate)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        lines = proc.stdout.splitlines()
        self.assertRegex(
            lines[0],
            r"^PASS parity max_rel=[^ ]+ max_abs=[^ ]+ worst_col=pnl_total$",
        )
        self.assertTrue(
            any(line.startswith("COL pnl_total max_rel=") for line in lines[1:]),
            proc.stdout,
        )

    def test_large_float_drift_fails_and_names_first_cell(self) -> None:
        golden = self.track("2026-01-02\t1\t1.0")
        candidate = self.track("2026-01-02\t1\t1.000001")
        proc = self.compare(golden, candidate)
        self.assertEqual(proc.returncode, 1)
        self.assertIn("FAIL", proc.stdout)
        self.assertIn("col=pnl_total", proc.stdout)
        self.assertIn("row=1", proc.stdout)

    def test_zero_reference_uses_absolute_tolerance(self) -> None:
        golden = self.track("2026-01-02\t1\t0.0")
        within = self.track("2026-01-02\t1\t5e-13")
        outside = self.track("2026-01-02\t1\t2e-12")

        pass_proc = self.compare(golden, within)
        self.assertEqual(pass_proc.returncode, 0, pass_proc.stdout + pass_proc.stderr)
        self.assertTrue(pass_proc.stdout.startswith("PASS parity "), pass_proc.stdout)

        fail_proc = self.compare(golden, outside)
        self.assertEqual(fail_proc.returncode, 1)
        self.assertIn("col=pnl_total", fail_proc.stdout)
        self.assertIn("row=1", fail_proc.stdout)

    def test_integer_cell_is_exact_even_if_candidate_looks_float_like(self) -> None:
        golden = self.track("2026-01-02\t1\t1.0")
        candidate = self.track("2026-01-02\t1.0\t1.0")
        proc = self.compare(golden, candidate)
        self.assertEqual(proc.returncode, 1)
        self.assertIn("col=n_open_lots", proc.stdout)
        self.assertIn("row=1", proc.stdout)

    def test_row_count_difference_fails(self) -> None:
        golden = self.track("2026-01-02\t1\t1.0", "2026-01-05\t2\t2.0")
        candidate = self.track("2026-01-02\t1\t1.0")
        proc = self.compare(golden, candidate)
        self.assertEqual(proc.returncode, 1)
        self.assertIn("FAIL row count", proc.stdout)

    def test_header_difference_fails(self) -> None:
        golden = self.track("2026-01-02\t1\t1.0")
        candidate = self.track(
            "2026-01-02\t1\t1.0",
            header="date\tn_open_lots\tpnl_gamma",
        )
        proc = self.compare(golden, candidate)
        self.assertEqual(proc.returncode, 1)
        self.assertIn("FAIL header", proc.stdout)

    def test_metadata_is_not_part_of_the_table_parity_gate(self) -> None:
        golden = self.track("2026-01-02\t1\t1.0")
        # Real track metadata includes result-derived final_nav/total_return.
        # ULP-class numeric changes can therefore move the preamble even when
        # every tabular cell remains within the promised parity band.
        candidate = golden.replace("strategy=fixture", "final_nav=1.000000000001")
        proc = self.compare(golden, candidate)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue(proc.stdout.startswith("PASS parity "), proc.stdout)


if __name__ == "__main__":
    unittest.main()
