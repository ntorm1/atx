"""Synthetic D3 producer tests. No downloads, warehouse, or real market payloads."""
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
import export_fundamental_fields as ex
import pit_fundamental_clock as fc

D = dt.date
CIK = "0000000001"
A = "0000099999-13-000001"  # filing agent prefix intentionally differs from issuer CIK
B = "0000099999-13-000002"
C = "0000099999-13-000003"


def evidence(accn=A, filed="2013-05-01", accepted="2013-05-01T16:00:00Z",
             published="2013-05-01T16:02:00Z", status="original-confirmed", revision=None):
    return fc.FilingEvidence.parse(dict(cik=CIK, accession=accn, filed=filed,
        accepted_at=accepted, published_at=published, revision_available_at=revision,
        revision_status=status, source_sha256="a" * 64, source_locator="synthetic:filing",
        accepted_raw=accepted or ""))


def fact(concept="Assets", value=100., accn=A, filed=D(2013, 5, 1), end=D(2013, 3, 31), start=None):
    tax = "dei" if concept == fc.DEI else "us-gaap"
    unit = "shares" if "Shares" in concept else "USD"
    return fc.Fact(concept, start, end, value, filed, "10-Q", accn, tax, unit)


def snapshots(facts, *proof):
    clocks = {(p.cik, p.accession): p for p in proof}
    return fc.snapshot_events(facts, CIK, clocks, D(2020, 1, 1), 180,
        knowledge_factory=ex.Knowledge, snapshot_fn=ex.snapshot, event_forms=ex.EVENT_FORMS)


class FundamentalAcceptanceV2(unittest.TestCase):
    def test_acceptance_public_revision_and_exact_modeled_fallback_are_distinct(self):
        item = evidence(status="revision-confirmed", revision="2013-05-03T17:00:00Z")
        clock = fc.resolve_clock(CIK, A, D(2013, 5, 1), {(CIK, A): item}, 180)
        self.assertEqual(clock["available_ns"], fc.stamp("2013-05-03T17:00:00Z"))
        self.assertEqual(clock["clock_kind"], 1)
        item = evidence(published=None)
        modeled = fc.resolve_clock(CIK, A, item.filed, {(CIK, A): item}, 180)
        self.assertEqual(modeled["available_ns"], item.accepted_ns + 180 * fc.NS)
        self.assertEqual(modeled["knowledge_clock_qualified"], 0)
        missing = fc.resolve_clock(CIK, A, item.filed, {}, 180)
        self.assertEqual(missing["available_ns"], fc.ns(item.filed) + 46 * 3600 * fc.NS)
        self.assertEqual(missing["fact_vintage_qualified"], 0)
        with self.assertRaises(ValueError):
            evidence(accepted="2013-05-01T16:00:00")
        with self.assertRaises(ValueError):
            evidence(published="2013-05-01T15:59:00Z")
        with self.assertRaises(ValueError):
            evidence(status="revision-confirmed")
        with self.assertRaises(ValueError):
            fc.resolve_clock(CIK, A, D(2013, 5, 2), {(CIK, A): item}, 180)

    def test_same_day_accessions_are_not_applied_before_their_own_publication(self):
        early = evidence()
        late = evidence(B, accepted="2013-05-01T19:00:00Z", published="2013-05-01T19:02:00Z")
        rows, _, _ = snapshots([fact(value=100.), fact(value=900., accn=B)], early, late)
        self.assertEqual([r["total_assets"] for r in rows], [100., 900.])
        self.assertEqual([r["available_ns"] for r in rows], [early.published_ns, late.published_ns])
        changed, _, _ = snapshots([fact(value=100.), fact(value=777., accn=B)], early, late)
        self.assertEqual(rows[0]["total_assets"], changed[0]["total_assets"])
        self.assertEqual(rows[0]["available_ns"], changed[0]["available_ns"])

    def test_inherited_modeled_or_unverified_knowledge_is_not_laundered(self):
        first = evidence(published=None)
        later = evidence(B, filed="2013-08-01", accepted="2013-08-01T16:00:00Z", published="2013-08-01T16:02:00Z")
        fs = [fact(), fact(value=200., accn=B, filed=D(2013, 8, 1), end=D(2013, 6, 30)),
              fact(fc.DEI, 50., B, D(2013, 8, 1), D(2013, 7, 25))]
        rows, shares, _ = snapshots(fs, first, later)
        self.assertEqual(rows[-1]["fact_vintage_qualified"], 1)
        self.assertEqual(rows[-1]["knowledge_clock_qualified"], 0)
        self.assertTrue(shares[0]["observed_public_admissible"])
        rows, _, _ = snapshots(fs, evidence(status="unverified"), later)
        self.assertEqual(rows[-1]["fact_vintage_qualified"], 0)
        self.assertEqual(rows[-1]["knowledge_clock_qualified"], 1)

    def test_simultaneous_conflict_withheld_and_later_qualified_correction_recovers(self):
        later = evidence(C, filed="2013-05-02", accepted="2013-05-02T16:00:00Z", published="2013-05-02T16:02:00Z")
        rows, _, audit = snapshots([fact(value=100.), fact(value=200., accn=B),
            fact(value=150., accn=C, filed=D(2013, 5, 2))], evidence(), evidence(B), later)
        self.assertEqual(len(rows), 2)
        self.assertTrue(math.isnan(rows[0]["total_assets"]))
        self.assertEqual(rows[0]["fact_vintage_qualified"], 0)
        self.assertEqual(rows[1]["total_assets"], 150.)
        self.assertEqual(rows[1]["fact_vintage_qualified"], 1)
        self.assertTrue(any(r.get("reason") == "conflicting_simultaneous_fact" for r in audit))

    def test_expired_unqualified_facts_do_not_permanently_taint_new_history(self):
        old = fact(accn=B, filed=D(2000, 5, 1), end=D(2000, 3, 31))
        old_clock = evidence(B, filed="2000-05-01", accepted="2000-05-01T16:00:00Z",
                             published="2000-05-01T16:02:00Z", status="unverified")
        rows, _, audit = snapshots([old, fact(value=200.)], old_clock, evidence())
        self.assertEqual(rows[-1]["fact_vintage_qualified"], 1)
        self.assertEqual(rows[-1]["total_assets"], 200.)
        self.assertTrue(any(r.get("expired_cells") == 1 for r in audit))

    def test_true_entity_shares_never_sum_and_diluted_accounting_stays_separate(self):
        diluted = "WeightedAverageNumberOfDilutedSharesOutstanding"
        fs = [fact(diluted, 75., start=D(2013, 1, 1)), fact(fc.DEI, 100.), fact(fc.DEI, 100.)]
        rows, shares, _ = snapshots(fs, evidence())
        self.assertEqual(rows[0]["shares_outstanding"], 75.)
        self.assertEqual(shares[0]["true_entity_shares"], 100.)
        _, conflict, _ = snapshots(fs + [fact(fc.DEI, 101.)], evidence())
        self.assertIsNone(conflict[0]["true_entity_shares"])
        _, unknown, _ = snapshots(fs, evidence(status="unverified"))
        self.assertIsNone(unknown[0]["true_entity_shares"])

    def test_conflicting_sue_history_is_unavailable_until_later_qualified_correction(self):
        ends = [D(year, month, day) for year in (2013, 2014, 2015)
                for month, day in ((3, 31), (6, 30), (9, 30), (12, 31))][:10]
        fs = []
        for q, end in enumerate(ends):
            start = D(end.year, ((end.month - 1) // 3) * 3 + 1, 1)
            fs.append(fact("NetIncomeLoss", 100. + q * q, A, D(2016, 5, 1), end, start))
        target = fs[4]
        fs.append(fact("NetIncomeLoss", 777., B, target.filed, target.end, target.start))
        fs.append(fact("NetIncomeLoss", target.val, C, D(2016, 5, 2), target.end, target.start))
        first = evidence(filed="2016-05-01", accepted="2016-05-01T16:00:00Z", published="2016-05-01T16:02:00Z")
        second = evidence(B, filed="2016-05-01", accepted="2016-05-01T16:00:00Z", published="2016-05-01T16:02:00Z")
        corrected = evidence(C, filed="2016-05-02", accepted="2016-05-02T16:00:00Z", published="2016-05-02T16:02:00Z")
        rows, _, _ = snapshots(fs, first, second, corrected)
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["fact_vintage_qualified"], 0)
        self.assertTrue(math.isnan(rows[0]["sue"]))
        self.assertEqual(rows[1]["fact_vintage_qualified"], 1)
        self.assertTrue(math.isfinite(rows[1]["sue"]))

    def test_v4_nonfinite_derived_seasonal_differences_are_unavailable(self):
        ends = [D(year, month, day) for year in (2013, 2014, 2015)
                for month, day in ((3, 31), (6, 30), (9, 30), (12, 31))][:10]
        fs = []
        for q, end in enumerate(ends):
            start = D(end.year, ((end.month - 1) // 3) * 3 + 1, 1)
            value = -1.e308 if q < 4 else 1.e308
            fs.append(fact("NetIncomeLoss", value, A, D(2016, 5, 1), end, start))
        clock = evidence(filed="2016-05-01", accepted="2016-05-01T16:00:00Z", published="2016-05-01T16:02:00Z")
        rows, _, _ = snapshots(fs, clock)
        self.assertEqual(len(rows), 1)
        self.assertTrue(math.isnan(rows[0]["sue"]))

    def test_actual_exporter_writes_hash_bound_v4_and_separate_true_shares(self):
        with tempfile.TemporaryDirectory(prefix="atx-d3-synthetic-") as owned:
            root = Path(owned); out = root / "export"
            body = {"cik": 1, "facts": {"us-gaap": {}, "dei": {}}}
            for f in [fact(), fact("StockholdersEquity", 10.), fact(fc.DEI, 125.),
                      fact("WeightedAverageNumberOfDilutedSharesOutstanding", 75., start=D(2013, 1, 1))]:
                row = dict(end=f.end.isoformat(), val=f.val, filed=f.filed.isoformat(), form=f.form, accn=f.accn)
                if f.start: row["start"] = f.start.isoformat()
                body["facts"][f.taxonomy][f.concept] = {"units": {f.unit: [row]}}
            zip_path = root / "synthetic.zip"
            with zipfile.ZipFile(zip_path, "w") as stream:
                stream.writestr("CIK0000000001.json", json.dumps(body))
            fact_hash = ex.sha256_file(zip_path)
            fact_proof = root / "facts.manifest.json"
            fact_proof.write_text(json.dumps(dict(schema="atx.sec-companyfacts-sealed/v1",
                seal_exclusive="2020-01-01", payload_sha256=fact_hash)), encoding="utf-8")
            clock_row = dict(cik=CIK, accession=A, filed="2013-05-01", accepted_at="2013-05-01T16:00:00Z",
                published_at="2013-05-01T16:02:00Z", revision_status="original-confirmed",
                source_sha256="a" * 64, source_locator="synthetic:filing")
            clocks = root / "clocks.json"
            clocks.write_text(json.dumps({"schema": "atx.sec-filing-clocks/v1", "records": [clock_row]}), encoding="utf-8")
            clock_proof = root / "clocks.manifest.json"
            clock_proof.write_text(json.dumps(dict(schema="atx.sec-filing-clocks-sealed/v1",
                seal_exclusive="2020-01-01", payload_sha256=ex.sha256_file(clocks), facts_payload_sha256=fact_hash)), encoding="utf-8")
            ctx = root / "context"; ctx.mkdir(); (ctx / "context.bin.manifest.json").write_text("{}", encoding="utf-8")
            links = root / "links"; links.mkdir(); (links / "manifest.json").write_text("{}", encoding="utf-8")
            context = SimpleNamespace(name="synthetic", path=ctx, instrument_ids=("7",),
                session_keys=(fc.stamp("2013-05-01T16:02:00Z"), fc.stamp("2013-05-02T22:00:00Z")))
            link = SimpleNamespace(sr_id="7", owner_id="SEC-CIK-0000000001", link_id="synthetic-proof", cik=CIK,
                valid_from=D(2013, 1, 1), valid_to=D(2014, 1, 1), available_at=D(2013, 1, 1),
                retired_from=None, retired_available_at=None, method="filing-bracket-v2", live_eligible=True, is_marker=False)
            argv = ["--companyfacts", str(zip_path), "--companyfacts-sealed-manifest", str(fact_proof),
                "--security-links", str(links), "--contexts", str(ctx), "--out", str(out),
                "--filing-clock-rule", "acceptance-v2", "--filing-clocks", str(clocks),
                "--filing-clocks-sealed-manifest", str(clock_proof), "--acceptance-delay-seconds", "180"]
            with mock.patch.object(ex, "load_contexts", return_value=[context]), mock.patch.object(
                    ex, "load_dated_id_bridge", return_value=({"7": [link]}, {}, {"schema": "synthetic"})), mock.patch.object(
                    ex, "load_id_bridge", side_effect=AssertionError("warehouse forbidden")):
                self.assertEqual(ex.main(argv), 0)
            manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["schema"], "atx.fundamental-fields-export/v4")
            self.assertFalse(manifest["plan_coverage_qualified"])
            for name, metadata in manifest["outputs"].items():
                self.assertEqual(metadata["sha256"], ex.sha256_file(out / name))
            self.assertEqual(manifest["recipe_sha256"], hashlib.sha256(json.dumps(manifest["recipe"],
                sort_keys=True, separators=(",", ":")).encode()).hexdigest())
            points = (out / "points.interval-v4.tsv").read_text(encoding="utf-8").splitlines()
            self.assertEqual(points[0], "ATX-FUNDAMENTAL-INTERVALS\t4")
            values = dict(zip(points[1].split("\t"), points[3].split("\t")))
            self.assertEqual(float(values["shares_outstanding"]), 75.)
            self.assertEqual(int(values["available_ns"]), fc.stamp(clock_row["published_at"]))
            share = json.loads((out / "shares.entity-v1.jsonl").read_text(encoding="utf-8"))
            self.assertEqual(share["true_entity_shares"], 125.)
            self.assertNotIn("market_cap", share)
            # A clock projection for a different fact vintage cannot publish anything.
            proof = json.loads(clock_proof.read_text(encoding="utf-8")); proof["facts_payload_sha256"] = "b" * 64
            clock_proof.write_text(json.dumps(proof), encoding="utf-8")
            argv[argv.index("--out") + 1] = str(root / "mismatch")
            with self.assertRaisesRegex(SystemExit, "another Company Facts"):
                ex.main(argv)
            self.assertFalse((root / "mismatch").exists())

    def test_legacy_writer_bytes_and_24_hour_clock_remain_explicit(self):
        link = SimpleNamespace(sr_id="7", owner_id="issuer", link_id="proof", valid_from=D(2013, 1, 1),
            valid_to=D(2014, 1, 1), available_at=D(2013, 1, 1), retired_from=None,
            retired_available_at=None, method="filing-bracket-v2", live_eligible=True, is_marker=False)
        rows = ex.company_snapshots([ex.Fact("Assets", None, D(2013, 3, 31), 100., D(2013, 5, 1), "10-Q", A)], D(2020, 1, 1))
        projected = ex.project_dated_snapshots(link, rows)
        self.assertEqual(projected[1]["available_ns"], fc.ns(D(2013, 5, 1)) + 24 * 3600 * fc.NS)
        with tempfile.TemporaryDirectory(prefix="atx-d3-legacy-") as owned:
            path = Path(owned) / "points.tsv"
            ex.write_interval_points(path, {"7": projected})
            expected = "ATX-FUNDAMENTAL-INTERVALS\t3\n" + "\t".join(ex.INTERVAL_KEYS + ex.RAW_FIELDS) + "\n"
            for row in projected:
                expected += "\t".join([str(row[k]) for k in ex.INTERVAL_KEYS] +
                    [repr(row[f]) if math.isfinite(row[f]) else "" for f in ex.RAW_FIELDS]) + "\n"
            self.assertEqual(path.read_bytes(), expected.encode())


if __name__ == "__main__":
    unittest.main()
