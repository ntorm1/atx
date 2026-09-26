"""Synthetic interval export integration; no DuckDB or external payloads."""
import contextlib
import csv
import datetime as dt
import io
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import export_fundamental_fields as ex

sl = ex._security_link_module()
D = dt.date
UTC = dt.timezone.utc
H = "b" * 64


def clock(day):
    return dt.datetime(2013, 1, day, 18, tzinfo=UTC)


def link(cik, begin, end, available, method="bracketed-filings-v1"):
    return sl.SecurityLink("link-" + str(cik) + "-" + method, "7", str(cik).zfill(10), "XYZ",
        D(2013, 1, begin), D(2013, 1, end), clock(1), clock(available), ("proof-" + str(cik),),
        method, "reviewer" if method == "dated-override-v1" else "",
        "dated original proof" if method == "dated-override-v1" else "")


def snapshot(value, period=D(2012, 12, 31), available=D(2013, 1, 3)):
    row = {f: math.nan for f in ex.RAW_FIELDS}
    row.update(book_equity=value, available_date=available, period_end=period)
    return row


def keys():
    return [ex._ns(dt.datetime(2013, 1, d, 22, tzinfo=UTC)) for d in range(2, 11)]


def company(cik, value, period="2012-12-31"):
    return {"cik": cik, "entityName": "Synthetic", "facts": {"us-gaap": {
        "StockholdersEquity": {"units": {"USD": [{"end": period, "val": value,
            "filed": "2013-01-02", "form": "10-K", "accn": "synthetic-" + str(cik)}]}}}}}


class SecurityLinkExportTest(unittest.TestCase):
    def test_sequential_issuers_expiry_and_separate_clocks(self):
        rows = ex.project_dated_snapshots(link(1, 1, 6, 3), [snapshot(10)])
        rows += ex.project_dated_snapshots(link(2, 6, 10, 7), [snapshot(20, D(2012, 9, 30))])
        values = ex.align_interval_values(rows, keys(), 0, 365, 550)
        got = [r["book_equity"] for r in values]
        self.assertEqual(got[1:4], [10, 10, 10])
        self.assertTrue(math.isnan(got[0]))
        self.assertTrue(math.isnan(got[4]))  # successor link not yet available
        self.assertEqual(got[5:8], [20, 20, 20])  # older fiscal period successor wins
        self.assertTrue(math.isnan(got[8]))  # explicit interval expiry
        lagged = ex.align_interval_values(rows, keys(), 2, 365, 550)
        self.assertTrue(math.isnan(lagged[2]["book_equity"]))
        self.assertEqual(lagged[3]["book_equity"], 10)
        self.assertEqual(lagged[5]["book_equity"], 20)  # no second lag on link clock

    def test_future_conflict_without_facts_withholds_then_override_restores(self):
        rows = ex.project_dated_snapshots(link(1, 1, 11, 2), [snapshot(10)])
        prefix = ex.align_interval_values(rows, keys(), 0, 365, 550)
        rows += ex.project_dated_snapshots(link(2, 1, 11, 6, "conflict-v1"), [snapshot(999)])
        changed = ex.align_interval_values(rows, keys(), 0, 365, 550)
        self.assertEqual(changed[1:4], prefix[1:4])
        self.assertTrue(all(math.isnan(r["book_equity"]) for r in changed[4:]))
        rows += ex.project_dated_snapshots(link(1, 1, 11, 8, "dated-override-v1"), [snapshot(10)])
        adjudicated = ex.align_interval_values(rows, keys(), 0, 365, 550)
        self.assertTrue(math.isnan(adjudicated[5]["book_equity"]))
        self.assertEqual(adjudicated[6]["book_equity"], 10)

    def test_default_export_writes_hash_bound_v2_without_warehouse(self):
        with tempfile.TemporaryDirectory(prefix="atx-d1-export-") as tmp:
            root = Path(tmp)
            links = [link(1, 1, 6, 3), link(2, 6, 10, 7)]
            bridge = root / "links"
            sl.write_link_artifact(bridge, links, [], {p: H for x in links for p in x.evidence_ids})
            payload = root / "synthetic.zip"
            with zipfile.ZipFile(payload, "w") as archive:
                for cik, value, period in [(1, 10, "2012-12-31"), (2, 20, "2012-09-30")]:
                    archive.writestr(f"CIK{cik:010}.json", json.dumps(company(cik, value, period)))
            receipt = root / "synthetic-sealed.json"
            receipt.write_text(json.dumps({"schema": "atx.sec-companyfacts-sealed/v1",
                "seal_exclusive": "2020-01-01", "payload_sha256": ex.sha256_file(payload)}))
            context = root / "context"
            context.mkdir()
            (context / "context.bin.manifest.json").write_text(json.dumps({"axes": {
                "instrument_namespace": "spiderrock.securityID", "instrument_ids": ["7"],
                "session_keys": keys()}}))
            output = root / "export"
            argv = ["--companyfacts", str(payload), "--companyfacts-sealed-manifest", str(receipt),
                    "--security-links", str(bridge), "--contexts", str(context), "--out", str(output),
                    "--lag-sessions", "0"]
            with mock.patch.dict(sys.modules, {"duckdb": None}), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(ex.main(argv), 0)
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual(manifest["identity_rule"], "dated-links-v2")
            self.assertFalse(manifest["plan_coverage_qualified"])
            self.assertIsNone(manifest["inputs"]["warehouse_bridge"])
            self.assertFalse((output / "points.csv").exists())
            for name, binding in manifest["outputs"].items():
                self.assertEqual(ex.sha256_file(output / name), binding["sha256"])
            lines = (output / "points.interval-v2.tsv").read_text().splitlines()
            self.assertEqual(lines[0], "ATX-FUNDAMENTAL-INTERVALS\t2")
            records = list(csv.DictReader(lines[1:], delimiter="\t"))
            self.assertEqual(len(records), 4)  # two identity markers and two fiscal snapshots
            self.assertEqual({r["owner_id"] for r in records}, {"SEC-CIK-0000000001", "SEC-CIK-0000000002"})
            audit = json.loads((output / "availability_audit.json").read_text())
            self.assertEqual(audit["contexts"][0]["finite_cells"]["book_equity"], 6)
            with self.assertRaises(SystemExit):
                ex.main(argv)  # immutable output

    def test_legacy_bridge_requires_opt_in_and_strict_receipt_is_mandatory(self):
        with self.assertRaisesRegex(ValueError, "explicit"):
            ex.load_id_bridge("never-opened.duckdb", {"7"})
        with self.assertRaisesRegex(SystemExit, "requires --security-links"):
            ex.main(["--companyfacts", "never-opened.zip", "--contexts", "unused", "--out", "unused-d1-test-output"])


if __name__ == "__main__":
    unittest.main()
