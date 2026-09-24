"""Tiny original/prepared archive fixtures; never reads the downloaded history."""
import argparse
import datetime as dt
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
import prepare_tickerhistory as qa
import audit_tickerhistory_reconciliation as audit

MEMBER = "tbltickerhistory_fixture.txt"
COLUMNS = ("tradingDate", "securityID", "ticker_tk", "todayTicker", "dn", "open", "high", "low", "close",
           "volume", "shares", "closePr", "closeUnadjPr", "returnFactor", "totalReturn", "cumulReturnFactor")
HEADER = ("\t".join(COLUMNS) + "\r\n").encode()
DATES = [f"2013-04-0{i}" for i in range(1, 6)]


def row(day, sid="1", **overrides):
    fields = dict(tradingDate=day, securityID=sid, ticker_tk="HIST", todayTicker="LATEST",
        dn=str(100 + int(day[-2:])), open="100", high="101", low="99", close="100.00",
        volume="1000", shares="100", closePr="100", closeUnadjPr="100", returnFactor="1",
        totalReturn="0", cumulReturnFactor="1")
    fields.update(overrides)
    return ("\t".join(fields[name] for name in COLUMNS) + "\r\n").encode()


def write_json(path, value):
    path.write_bytes(json.dumps(value, sort_keys=True, indent=2, allow_nan=False).encode() + b"\n")


def key(day):
    return str((dt.date.fromisoformat(day) - dt.date(1970, 1, 1)).days * audit.DAY_NS)


class ReconciliationAuditTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def artifact(self, name, dates, recipe, parents):
        # Real small APNL layout; values are deliberately synthetic, not a new
        # oracle for preparation. The auditor verifies payload identity, not values.
        fields = ["close", "raw_close", "volume"]
        payload = struct.pack("<IIQQQ", 0x4C4E5041, 1, len(dates), 1, 3)
        for field in fields:
            payload += struct.pack("<I", len(field)) + field.encode()
        payload += struct.pack("<" + "d" * len(dates) * 3, *([100.0] * len(dates) * 3))
        payload += b"\1" * len(dates)
        fnv = 14695981039346656037
        for byte in payload:
            fnv = ((fnv ^ byte) * 1099511628211) & ((1 << 64) - 1)
        payload += struct.pack("<Q", fnv)
        path = self.root / (name + ".bin")
        path.write_bytes(payload)
        axes = {"date_encoding": "UnixNanoseconds", "date_semantics": "session-label-not-availability",
                "instrument_namespace": "spiderrock.securityID", "instrument_ids": ["1"],
                "original_instrument_indices": ["7"], "session_keys": [key(day) for day in dates]}
        document = {"schema": "atx.panel-artifact", "schema_version": 1, "axes": axes,
            "shape": {"dates": str(len(dates)), "instruments": "1", "fields": fields},
            "recipe": recipe, "parents": [{"role": role, "sha256": sha} for role, sha in sorted(parents.items())],
            "payload": {"format": "APNLv1", "size_bytes": str(len(payload)), "sha256": audit.digest(payload),
                        "fnv1a64": f"{fnv:016x}"}}
        document["hashes"] = {"axes_sha256": audit.digest(audit.canonical(axes)),
            "recipe_sha256": audit.digest(recipe.encode()), "parents_sha256": audit.digest(audit.canonical(document["parents"]))}
        document["artifact_id"] = audit.digest(b"atx-panel-artifact-v1\n" + audit.canonical(document))
        manifest = Path(str(path) + ".manifest.json")
        write_json(manifest, document)
        return manifest, document

    def inputs(self, originals, gaps=("2013-04-03",), tail=False):
        source = self.root / "original.zip"
        rows = originals + [row(day, "2") for day in DATES]
        if tail:
            rows += [row("2014-01-02", "2")]
        rows.sort(key=lambda raw: raw.split(b"\t", 1)[0])
        with zipfile.ZipFile(source, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr(MEMBER, HEADER + b"".join(rows))
        prepared = self.root / "prepared"
        prep = qa.prepare(source, prepared, dt.date(2013, 4, 1), dt.date(2013, 4, 5))
        prep_sha = qa.sha256(prepared / "manifest.json")
        ingestion = {"schema": "atx-ingestion-v1", "status": "complete", "input": prep["accepted"],
            "preparation": {"manifest_sha256": prep_sha, "original_source_sha256": prep["source"]["sha256"]}}
        ingestion_path = self.root / "ingestion.json"
        write_json(ingestion_path, ingestion)
        context_recipe = json.dumps({"start_inclusive_nanos": key(DATES[0]), "end_exclusive_nanos": key("2013-04-06")})
        context_path, context = self.artifact("context", DATES, context_recipe,
            {"preparation_manifest": prep_sha, "preparation_declared_original_zip": prep["source"]["sha256"],
             "ingestion_input_zip": prep["accepted"]["sha256"], "ingestion_manifest": qa.sha256(ingestion_path)})
        recipe = {"evaluation_start": DATES[0], "evaluation_end_exclusive": "2013-04-06", "fit_kind": "unfit-constant-weights"}
        evaluation_path, evaluation = self.artifact("evaluation", DATES, json.dumps(recipe),
                                                   {"source-context": context["artifact_id"]})
        books_path, books = self.artifact("books", [DATES[0]], "synthetic scheduled book",
                                         {"research": evaluation["artifact_id"]})
        gap_rows = [{"evaluation_observation": DATES.index(day), "context_observation": DATES.index(day),
            "session_key_ns": key(day), "session_date_utc": day, "instrument_index": 0, "security_id": "1",
            "required_reasons": ["mark-existing-position"], "invalid_close": "NaN",
            "coverage_after_first_missing_mark": i > 0} for i, day in enumerate(gaps)]
        inventory = {"schema": audit.GAP_SCHEMA, "scope": "After first failure counterfactual coverage, not continued replay",
            "evaluation_artifact_id": evaluation["artifact_id"], "books_artifact_id": books["artifact_id"],
            "missing_required_cells": len(gap_rows), "affected_security_ids": ["1"],
            "first_missing_required_cell": gap_rows[0], "gaps": gap_rows}
        gaps_path = self.root / "gaps.json"
        write_json(gaps_path, inventory)
        attempt = self.root / "attempt"
        attempt.mkdir()
        request = {"status": "started", "recipe": recipe, "source_context_artifact_id": context["artifact_id"],
                   "source_context_payload_sha256": context["payload"]["sha256"]}
        write_json(attempt / "request.json", request)
        failure = dict(request, status="failed", error="replay: missing/nonpositive required close at period=" +
            str(gap_rows[0]["evaluation_observation"]) + " instrument=0 session_key_ns=" + key(gaps[0]) + " security_id=1")
        write_json(attempt / "failure.json", failure)
        oracle_path = self.root / "oracle.json"
        write_json(oracle_path, {"status": "passed", "source": {"sha256": prep["accepted"]["sha256"]},
                                "panel": {"sha256": context["payload"]["sha256"]}})
        return argparse.Namespace(source=source, preparation_manifest=prepared / "manifest.json", gaps=gaps_path,
            attempt_dir=attempt, context_manifest=context_path, evaluation_manifest=evaluation_path,
            books_manifest=books_path, ingestion_manifest=ingestion_path, source_fidelity_receipt=oracle_path,
            output_dir=self.root / "audit")

    def test_absent_source_is_unknown_with_exact_neighbors_and_partial_crc_scope(self):
        options = self.inputs([row(day) for day in DATES if day != "2013-04-03"], tail=True)
        report = audit.audit(options)
        gap = report["gaps"][0]
        self.assertEqual(gap["classification"], "source_row_absent")
        self.assertEqual(gap["economic_status"], "unverified")
        self.assertEqual(gap["neighbors"]["previous"], ("2013-04-02", "1"))
        self.assertEqual(gap["neighbors"]["next"], ("2013-04-04", "1"))
        self.assertEqual(gap["previous_to_current"]["status"], "not_comparable")
        self.assertFalse(report["original_scan"]["member_crc_verified"])
        self.assertTrue(report["accepted_scan"]["member_crc_verified"])
        self.assertEqual(report["required_gap_count"], 1)
        with self.assertRaises(FileExistsError):
            audit.audit(options)

    def test_duplicate_candidates_are_all_preserved_and_never_compared(self):
        candidate = row("2013-04-03", "0001", close="100.5")
        options = self.inputs([row(day) for day in DATES] + [candidate])
        report = audit.audit(options)
        group = next(group for group in report["groups"] if group["date"] == "2013-04-03")
        self.assertEqual(len(group["original_rows"]), 2)
        self.assertEqual(group["accepted_rows"], [])
        for entry in group["original_rows"]:
            self.assertEqual(entry["qa_reasons"], ["duplicate_positive_date_security_id"])
        self.assertEqual(report["gaps"][0]["previous_to_current"]["status"], "not_comparable")
        with zipfile.ZipFile(options.output_dir / "evidence.zip") as archive:
            content = archive.read("original_rows.tsv")
            self.assertTrue(content.startswith(HEADER))
            self.assertIn(candidate, content)
            self.assertIn(row("2013-04-03"), content)

    def test_bad_ohl_valid_close_stays_quarantined_shares_remain_flags(self):
        original = row("2013-04-03", low="-99", shares="0")
        options = self.inputs([original if day == "2013-04-03" else row(day) for day in DATES])
        report = audit.audit(options)
        gap = report["gaps"][0]
        self.assertEqual(gap["classification"], "quarantined_by_qa_v1")
        group = next(group for group in report["groups"] if group["date"] == "2013-04-03")
        self.assertEqual(group["original_rows"][0]["qa_flags"], ["zero_shares"])
        self.assertEqual(group["original_rows"][0]["values"]["low"], "-99")
        self.assertEqual(gap["previous_to_current"]["metrics"]["adjusted_return"]["residual"], 0)
        self.assertEqual(gap["economic_status"], "unverified")

    def test_split_factor_contradiction_is_diagnostic_and_never_repaired(self):
        original = row("2013-04-03", open="10", high="11", low="9", close="10", closePr="10", returnFactor="0.1")
        options = self.inputs([original if day == "2013-04-03" else row(day) for day in DATES])
        report = audit.audit(options)
        metrics = report["gaps"][0]["previous_to_current"]["metrics"]
        self.assertAlmostEqual(metrics["cumulative_daily_factor"]["residual"], -0.9)
        self.assertAlmostEqual(metrics["adjusted_return"]["residual"], -0.9)
        self.assertEqual(metrics["reported_return"]["residual"], 0)
        self.assertEqual(report["gaps"][0]["classification"], "accepted_row_present_missing_panel_requires_investigation")
        self.assertEqual(report["gaps"][0]["economic_status"], "unverified")
        self.assertEqual(qa.sha256(options.source), report["inputs"]["original_zip"]["sha256"])

        field = {name: i for i, name in enumerate(COLUMNS)}
        for price, factor in (("1e308", "10"), ("1e-200", "1e-200")):
            previous = {"values": row("2013-04-02", close=price, cumulReturnFactor=factor).rstrip().split(b"\t"),
                        "date": "2013-04-02", "ordinal": 1, "qa_reasons": []}
            current = {"values": row("2013-04-03").rstrip().split(b"\t"),
                       "date": "2013-04-03", "ordinal": 2, "qa_reasons": []}
            metric = audit.residuals([previous], [current], field)["metrics"]["adjusted_return"]
            self.assertEqual(metric["status"], "not_comparable")
            self.assertIsNone(metric["residual"])

    def test_quarantined_original_predecessor_is_not_skipped_and_dn_gap_is_unknown(self):
        rows = [row(day, close="100.5", low="-1") if day == "2013-04-02" else row(day) for day in DATES]
        options = self.inputs(rows, gaps=("2013-04-03", "2013-04-04"))
        report = audit.audit(options)
        pair = report["gaps"][0]["previous_to_current"]
        self.assertEqual(pair["previous_qa_reasons"], ["invalid_or_nonpositive_ohlc"])
        self.assertAlmostEqual(pair["metrics"]["prior_close_absolute"]["residual"], -0.5)
        field = {name: i for i, name in enumerate(COLUMNS)}
        p = {"values": row("2013-04-01").rstrip().split(b"\t"), "date": "2013-04-01"}
        t = {"values": row("2013-04-03").rstrip().split(b"\t"), "date": "2013-04-03"}
        self.assertEqual(audit.residuals([p], [t], field)["reasons"], ["original-dn-not-consecutive-or-invalid"])
        self.assertEqual(report["required_gap_count"], 2)

    def test_rounding_is_reported_and_manifest_and_evidence_hashes_reproduce(self):
        options = self.inputs([row(day, totalReturn="0.0000002") if day == "2013-04-03" else row(day) for day in DATES])
        report = audit.audit(options)
        residual = report["gaps"][0]["previous_to_current"]["metrics"]["reported_return"]
        self.assertEqual(residual["residual"], -2e-7)
        self.assertEqual(residual["absolute_bin"], "le_1e-06")
        self.assertEqual(qa.sha256(options.output_dir / "evidence.zip"), report["evidence"]["sha256"])
        identity = report.pop("audit_id")
        self.assertEqual(identity, audit.digest(b"tickerhistory-reconciliation-audit-v1\n" + audit.canonical(report)))
        options.output_dir = self.root / "repeat"
        self.assertEqual(audit.audit(options)["audit_id"], identity)

    def test_wrong_gap_axis_and_evidence_bound_fail_without_completion(self):
        options = self.inputs([row(day) for day in DATES])
        inventory = json.loads(options.gaps.read_bytes())
        inventory["gaps"][0]["context_observation"] = 1
        write_json(options.gaps, inventory)
        with self.assertRaisesRegex(ValueError, "axes mismatch"):
            audit.audit(options)
        self.assertTrue((options.output_dir / "failure.json").exists())
        self.assertFalse((options.output_dir / "manifest.json").exists())
        budget = audit.EvidenceBudget()
        with self.assertRaisesRegex(ValueError, "bound exceeded"):
            budget.retain([{"raw": b"x" * (audit.MAX_EVIDENCE_BYTES + 1)}])

    def test_failure_identity_does_not_accept_numeric_prefixes(self):
        options = self.inputs([row(day) for day in DATES])
        failure_path = options.attempt_dir / "failure.json"
        failure = json.loads(failure_path.read_bytes())
        failure["error"] += "0"
        write_json(failure_path, failure)
        with self.assertRaisesRegex(ValueError, "Failure diagnostic"):
            audit.audit(options)
        self.assertFalse((options.output_dir / "manifest.json").exists())


if __name__ == "__main__":
    unittest.main()
