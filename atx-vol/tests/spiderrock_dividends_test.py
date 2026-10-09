#!/usr/bin/env python3
"""Gate for the `ddiv` -> discrete-schedule emitter (tools/spiderrock_dividends.py).

The emitter mirrors the validated C++ reconstructor (tools/oracle_dividends.*),
so what is pinned here is exactly what that mirror promises:

  * a CLEAN step function differences back to its dividends, each placed at the
    UPPER BRACKET expiry (never the bracket midpoint);
  * an underlier whose `ddiv` never moves recovers an EMPTY schedule and is
    reported as "pays none" -- an ANSWER, not a refusal;
  * an underlier that violates the step shape is refused WHOLE, is counted by
    reason, and contributes NO rows. That distinction is the point: a refusal
    and a dividend-free name produce the same empty schedule and mean opposite
    things.

Pure tmp-dir fixtures; no store access, no network.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import unittest

import pyarrow as pa
import pyarrow.parquet as pq

TOOLS = pathlib.Path(__file__).parents[1] / "tools"
SCRIPT = TOOLS / "spiderrock_dividends.py"
sys.path.insert(0, str(TOOLS))

import spiderrock_dividends as srd  # noqa: E402  (path must be set first)

DATE = "2026-08-14"
BUCKET = "1030"

# One tiny expiry ladder shared by the fixtures: (years, YYYY-MM-DD). Spacing is
# irrelevant to the algorithm -- only the ORDER and the `ddiv` values are.
LADDER = [
    (0.0416, "2026-08-28"),
    (0.1174, "2026-09-25"),
    (0.3490, "2026-12-18"),
    (0.5937, "2027-03-19"),
]


def rows_for(ddivs: list[float]) -> list[tuple[float, float, str]]:
    """One row per ladder rung carrying `ddivs[i]`."""
    assert len(ddivs) == len(LADDER)
    return [(years, ddivs[i], expiry) for i, (years, expiry) in enumerate(LADDER)]


def write_store(root: pathlib.Path, per_underlier: dict[str, list[float]]) -> pathlib.Path:
    """Write a one-partition store slice, two strikes per (underlier, expiry).

    Two strikes because a real chain repeats `ddiv` across every strike of an
    expiry, and the collapse step has to be exercised on that shape rather than
    on an already-unique ladder.
    """
    partition = root / f"date={DATE}" / f"bucket_et={BUCKET}"
    partition.mkdir(parents=True, exist_ok=True)
    tk, yr, mn, dy, years, ddiv = [], [], [], [], [], []
    for underlier, ddivs in per_underlier.items():
        for row_years, row_ddiv, expiry in rows_for(ddivs):
            for _strike in (100.0, 105.0):
                y, m, d = (int(part) for part in expiry.split("-"))
                tk.append(underlier)
                yr.append(y)
                mn.append(m)
                dy.append(d)
                years.append(row_years)
                ddiv.append(row_ddiv)
    table = pa.table(
        {
            "undSecKey_tk": pa.array(tk, pa.string()),
            "okey_yr": pa.array(yr, pa.int64()),
            "okey_mn": pa.array(mn, pa.int64()),
            "okey_dy": pa.array(dy, pa.int64()),
            "years": pa.array(years, pa.float64()),
            "ddiv": pa.array(ddiv, pa.float64()),
        }
    )
    pq.write_table(table, partition / "00000000.parquet")
    return partition


class ReconstructTest(unittest.TestCase):
    def test_clean_step_recovers_each_dividend_at_its_upper_bracket(self):
        # Flat 0, then +1.50 at the 3rd rung and +1.50 at the 4th.
        schedule, n_expiries = srd.reconstruct(rows_for([0.0, 0.0, 1.50, 3.00]))
        self.assertEqual(n_expiries, len(LADDER))
        self.assertEqual(len(schedule), 2)
        # The UPPER bracket: the first expiry whose `ddiv` included the dividend,
        # not the midpoint between it and the previous one.
        self.assertEqual(schedule[0][0], "2026-12-18")
        self.assertEqual(schedule[1][0], "2027-03-19")
        self.assertAlmostEqual(schedule[0][1], 1.50, places=12)
        self.assertAlmostEqual(schedule[1][1], 1.50, places=12)

    def test_dividend_already_accrued_at_the_earliest_expiry_is_emitted_there(self):
        # The differencing baseline is 0, so a positive `ddiv` at the FIRST rung
        # is a real dividend whose upper bracket is that rung.
        schedule, _ = srd.reconstruct(rows_for([2.00, 2.00, 2.00, 2.00]))
        self.assertEqual(len(schedule), 1)
        self.assertEqual(schedule[0][0], LADDER[0][1])
        self.assertAlmostEqual(schedule[0][1], 2.00, places=12)

    def test_flat_zero_ddiv_is_an_empty_schedule_not_a_refusal(self):
        schedule, n_expiries = srd.reconstruct(rows_for([0.0, 0.0, 0.0, 0.0]))
        self.assertEqual(schedule, [])
        self.assertEqual(n_expiries, len(LADDER))

    def test_rounding_noise_below_the_flat_tolerance_does_not_refuse(self):
        noise = srd.DDIV_FLAT_TOL / 10.0
        schedule, _ = srd.reconstruct(rows_for([0.0, noise, 0.0, noise]))
        self.assertEqual(schedule, [])

    def test_decreasing_ddiv_is_refused_whole_as_non_monotone(self):
        with self.assertRaises(srd.Refused) as caught:
            srd.reconstruct(rows_for([0.0, 3.00, 1.50, 1.50]))
        self.assertEqual(caught.exception.reason, srd.NON_MONOTONE_DDIV)
        self.assertAlmostEqual(caught.exception.years, LADDER[2][0], places=12)

    def test_two_ddiv_answers_at_one_expiry_are_refused_as_ambiguous(self):
        rows = rows_for([0.0, 0.0, 1.50, 1.50])
        # A second row at the SAME `years` disagreeing on `ddiv` -- the real
        # vendor defect this reason exists for (observed on NVDA 2026-12-18).
        rows.append((LADDER[2][0], 0.0, LADDER[2][1]))
        with self.assertRaises(srd.Refused) as caught:
            srd.reconstruct(rows)
        self.assertEqual(caught.exception.reason, srd.AMBIGUOUS_DDIV_AT_EXPIRY)

    def test_jump_between_the_two_tolerances_is_refused_not_emitted(self):
        # Too large to be the column's rounding noise, far too small to be cash.
        between = (srd.DDIV_FLAT_TOL + srd.MIN_DIVIDEND_JUMP) / 2.0
        with self.assertRaises(srd.Refused) as caught:
            srd.reconstruct(rows_for([0.0, 0.0, between, between]))
        self.assertEqual(caught.exception.reason, srd.NON_POSITIVE_JUMP)

    def test_non_finite_input_is_refused_before_anything_is_sorted(self):
        with self.assertRaises(srd.Refused) as caught:
            srd.reconstruct(rows_for([0.0, float("nan"), 1.50, 1.50]))
        self.assertEqual(caught.exception.reason, srd.NON_FINITE_INPUT)

    def test_accrual_invariant_holds_against_the_original_ddiv_column(self):
        # The invariant reconstruction is FOR: summing the schedule up to an
        # expiry reproduces that expiry's own `ddiv`.
        ddivs = [0.0, 1.25, 1.25, 3.75]
        schedule, _ = srd.reconstruct(rows_for(ddivs))
        for i, (_, expiry) in enumerate(LADDER):
            self.assertAlmostEqual(srd.accrued(schedule, expiry), ddivs[i], places=12)


class EmitterCliTest(unittest.TestCase):
    def test_writes_only_reconstructed_underliers_and_reports_the_rest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            partition = write_store(
                root / "store",
                {
                    "AAA": [0.0, 0.0, 1.50, 3.00],  # clean step: two dividends
                    "BBB": [0.0, 0.0, 0.0, 0.0],  # pays nothing
                    "CCC": [0.0, 3.00, 1.50, 1.50],  # step violated: refused
                },
            )
            out = root / "divs.parquet"
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), "--partition", str(partition), "--out", str(out)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)

            table = pq.ParquetFile(out).read()
            self.assertEqual(
                [f.name for f in table.schema], ["underlying", "ex_date", "amount"]
            )
            rows = list(
                zip(
                    table["underlying"].to_pylist(),
                    table["ex_date"].to_pylist(),
                    table["amount"].to_pylist(),
                )
            )
            # Only AAA reaches the file: BBB has nothing to say and CCC was
            # refused whole rather than partially reconstructed.
            self.assertEqual([r[0] for r in rows], ["AAA", "AAA"])
            self.assertEqual([r[1] for r in rows], ["2026-12-18", "2027-03-19"])

            # Both non-emitting names are still NAMED, and distinguishably so.
            self.assertIn("BBB", proc.stderr)
            self.assertIn("pays none", proc.stderr)
            self.assertIn("CCC", proc.stderr)
            self.assertIn(srd.NON_MONOTONE_DDIV, proc.stderr)
            self.assertIn(f"{srd.NON_MONOTONE_DDIV}=1", proc.stderr)

    def test_fail_on_refusal_turns_a_refused_underlier_into_a_failing_exit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            partition = write_store(root / "store", {"CCC": [0.0, 3.00, 1.50, 1.50]})
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), "--partition", str(partition), "--fail-on-refusal"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 1, proc.stderr)

    def test_recovering_nothing_at_all_is_its_own_exit_code(self):
        # An empty schedule is what omitting --dividends already does, so a run
        # that produced none must not report success.
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            partition = write_store(root / "store", {"BBB": [0.0, 0.0, 0.0, 0.0]})
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), "--partition", str(partition)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 3, proc.stderr)

    def test_a_non_partition_directory_is_a_usage_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), "--partition", tmp],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 2, proc.stderr)

    def test_a_partition_with_no_parquet_is_refused_not_read_as_no_dividends(self):
        with tempfile.TemporaryDirectory() as tmp:
            partition = pathlib.Path(tmp) / f"date={DATE}" / f"bucket_et={BUCKET}"
            partition.mkdir(parents=True)
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), "--partition", str(partition)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 1, proc.stderr)


if __name__ == "__main__":
    unittest.main()
