"""Orchestration/receipt controls; fixed twelve grids are replaced by explicit stubs."""
import copy
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

import lcp_zero_rate_run as runner
from test_lcp_zero_rate_control import MODEL, completed_work


class OutputControls(unittest.TestCase):
    def test_dirty_source_refuses_before_creating_an_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)/"new"
            with patch.object(runner, "git", return_value=b" M changed.py\n") as git:
                with self.assertRaises(ValueError):
                    runner.source_snapshot(output)
            self.assertFalse(output.exists())
            git.assert_called_once_with("status", "--porcelain")

    def test_existing_output_is_never_replaced(self):
        original_git = runner.git
        def fake_clean(*args):
            return b"" if args == ("status", "--porcelain") else original_git(*args)
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            retained = output/"retained.txt"
            retained.write_bytes(b"unchanged")
            with patch.object(runner, "git", side_effect=fake_clean):
                with self.assertRaises(FileExistsError):
                    runner.source_snapshot(output)
            self.assertEqual(retained.read_bytes(), b"unchanged")
            self.assertEqual(list(output.iterdir()), [retained])

    def test_wrong_mode_cannot_invoke_baseline_or_compute(self):
        with patch.object(runner, "baseline") as baseline:
            for mode in (None, "original", True):
                with self.assertRaises(ValueError):
                    runner.run(Path("unused"), Path("unused"), mode)
            baseline.assert_not_called()

    def test_checksum_valid_failed_vector_mismatch_and_shape_refuse(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            name = "dirk_l-time-coarse-failure.npz"
            path = directory/name
            arrays = dict(lower=np.zeros(1), diagonal=np.ones(2), upper=np.zeros(1),
                          rhs=np.ones(2), payoff=np.zeros(2), initial=np.zeros(2),
                          last_checked_values=np.zeros(2))
            row = dict(scheme="dirk_l", label="time-coarse", options=dict(nx=4),
                       stage_work=[dict(status="refused", work=dict(last_checked_residual=1.))])
            def write():
                np.savez_compressed(path, **arrays)
                row["failure_snapshot"] = dict(file=name, sha256=runner.digest(path))
            write()
            runner.verify_failure_snapshot(directory, row)
            row["stage_work"][0]["work"]["last_checked_residual"] = .5
            with self.assertRaises(ValueError):
                runner.verify_failure_snapshot(directory, row)
            row["stage_work"][0]["work"]["last_checked_residual"] = 1.
            arrays["rhs"] = np.ones(2, dtype=np.float32)
            write()
            with self.assertRaises(ValueError):
                runner.verify_failure_snapshot(directory, row)
            row["failure_snapshot"]["file"] = "../outside.npz"
            with self.assertRaises(ValueError):
                runner.verify_failure_snapshot(directory, row)


class HistoricalControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = os.environ.get("ATX_LCP_BASELINE")
        if source is None:
            raise unittest.SkipTest("Explicit immutable v2 baseline required")
        cls.source = Path(source)

    def test_real_immutable_baseline_revalidates_without_any_pde_execution(self):
        corpus, rows, decisions = runner.baseline(self.source)
        self.assertEqual(len(corpus["cases"]), 715)
        self.assertEqual(len(rows), 12)
        self.assertEqual(sum(r["status"] == "computed" for r in rows), 6)
        self.assertTrue(all(d["status"] == "unresolved" for d in decisions.values()))
        with tempfile.TemporaryDirectory() as temporary:
            copy_dir = Path(temporary)/"historical"
            runner.copy_baseline(self.source, copy_dir)
            self.assertEqual(runner.baseline(copy_dir), (corpus, rows, decisions))
            with (copy_dir/"manifest.json").open("ab") as changed:
                changed.write(b"\n")
            with self.assertRaises(ValueError):
                runner.baseline(copy_dir)

    def test_actual_executor_keeps_all_requests_and_flushes_after_a_refusal(self):
        historical = [json.loads(line) for line in
                      (self.source/"outcomes.jsonl").read_text().splitlines()]
        old = [r for r in historical if r["id"] == MODEL["id"]]
        seen = []
        class Stream(io.StringIO):
            flushes = 0
            def flush(self):
                self.flushes += 1
                return super().flush()
        def fake_reference(mode, collector):
            self.assertEqual(mode, "one_correction")
            index = len(seen)
            seen.append(index)
            def solve(model, **options):
                row = old[index]
                self.assertEqual(model, MODEL)
                self.assertEqual(options, dict(row["options"], scheme=row["scheme"]))
                if row["status"] == "computed":
                    collector.records = completed_work(row)
                    return copy.deepcopy(row["result"])
                collector.records = [dict(stage=1, status="unobserved_failure", work=None,
                                          exception_type="ArithmeticError", detail=row["detail"])]
                raise ArithmeticError(row["detail"])
            return solve
        stream = Stream()
        with tempfile.TemporaryDirectory() as temporary, \
                patch("lcp_stage_adapter.private_reference", side_effect=fake_reference):
            rows = runner.execute_requests(Path(temporary), stream)
        self.assertEqual(len(seen), 12)
        self.assertEqual(stream.flushes, 12)
        self.assertEqual([json.loads(line) for line in stream.getvalue().splitlines()], rows)
        self.assertEqual(sum(r["status"] == "refused" for r in rows), 6)
        summary = runner.summarize_control(rows)
        self.assertEqual(summary["requested"], 12)
        self.assertFalse(summary["solver_controls_passed"])
        self.assertFalse(summary["reference_decisions_updated"])

    def test_complete_negative_receipt_and_hash_valid_mutations(self):
        corpus, old, decisions = runner.baseline(self.source)
        rows = copy.deepcopy(old)
        for row in rows:
            row.update(lcp_mode="one_correction", failure_snapshot=None)
            row["stage_work"] = completed_work(row) if row["status"] == "computed" else [
                dict(stage=1, status="unobserved_failure", work=None,
                     exception_type="ArithmeticError", detail=row["detail"])]
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            runner.copy_baseline(self.source, output/"historical-v2")
            (output/"sources").mkdir()
            frozen = {}
            for name in runner.SOURCES:
                raw = (runner.HERE/name).read_bytes()
                (output/"sources"/name).write_bytes(raw)
                frozen[runner.PREFIX+name] = raw
            stream = output/"outcomes.jsonl"
            stream.write_text("".join(json.dumps(row)+"\n" for row in rows), encoding="utf-8")
            summary = runner.summarize_control(rows)
            summary.update(historical_outcomes=old, historical_decisions=decisions,
                other_cold_fields={field: "uncomputed" for field in
                                   ("vega", "theta", "rho", "vanna", "volga", "charm")})
            runner.json_write(output/"summary.json", summary)
            # Explicit protocol-only Git object stub; no claim of a new native/PDE receipt.
            source = "f"*40
            real_git = runner.git
            def fixture_git(*args):
                if args[0] == "show" and args[1].startswith(source+":"):
                    return frozen[args[1].split(":", 1)[1]]
                return real_git(*args)
            manifest = dict(schema="atx-zero-rate-lcp-control-v1", source=source,
                source_hashes={runner.PREFIX+name: runner.digest(output/"sources"/name)
                               for name in runner.SOURCES},
                lcp_mode=runner.MODE, primitive_source=runner.PRIMITIVE,
                declaration_commit=real_git("rev-parse", runner.PLAN).decode().strip(),
                completed=True, source_unchanged=True, history_unchanged=True,
                reference_decisions_updated=False, full_144_run=False, accuracy_established=False,
                full_bundle_qualified=False, timing_qualified=False,
                baseline_hashes=runner.BASELINE_HASHES, requested_results=12, input_count=715,
                selected_ids=[MODEL["id"]], requests=runner.fixed_requests(),
                uncomputed_ids=[case["id"] for case in corpus["cases"]
                                if case["id"] != MODEL["id"]],
                computed=6, refused=6, outcomes_sha256=runner.digest(stream),
                summary_sha256=runner.digest(output/"summary.json"),
                python="protocol only", executable="protocol only", platform="protocol only",
                numpy="protocol only", scipy="protocol only",
                started_utc="2026-10-24T00:00:00+00:00", finished_utc="2026-10-24T00:00:01+00:00")
            runner.json_write(output/"manifest.json", manifest)
            with patch.object(runner, "git", side_effect=fixture_git):
                self.assertEqual(runner.verify_run(output), summary)
                for field, value in (("completed", False), ("reference_decisions_updated", True),
                                     ("computed", 12), ("uncomputed_ids", [])):
                    wrong = dict(manifest, **{field: value})
                    runner.json_write(output/"manifest.json", wrong)
                    with self.subTest(field=field), self.assertRaises(ValueError):
                        runner.verify_run(output)
                runner.json_write(output/"manifest.json", manifest)
                altered = dict(summary, solver_controls_passed=True)
                runner.json_write(output/"summary.json", altered)
                wrong = dict(manifest, summary_sha256=runner.digest(output/"summary.json"))
                runner.json_write(output/"manifest.json", wrong)
                with self.assertRaises(ValueError):
                    runner.verify_run(output)
                runner.json_write(output/"summary.json", summary)
                # Even a self-consistent source/Git hash must not relabel a different
                # primitive as the reviewed immutable17ab implementation.
                key = runner.PREFIX+"lcp_correction.py"
                frozen[key] += b"\n# different primitive revision\n"
                (output/"sources/lcp_correction.py").write_bytes(frozen[key])
                wrong = copy.deepcopy(manifest)
                wrong["source_hashes"][key] = runner.digest(output/"sources/lcp_correction.py")
                runner.json_write(output/"manifest.json", wrong)
                with self.assertRaises(ValueError):
                    runner.verify_run(output)


if __name__ == "__main__":
    unittest.main()
