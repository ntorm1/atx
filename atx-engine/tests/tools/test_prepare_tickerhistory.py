"""Behavioral input-QA checks; uses tiny synthetic ZIPs only."""
import datetime as dt
import importlib.util
import hashlib
import pathlib
import tempfile
import unittest
import zipfile

TOOL = pathlib.Path(__file__).resolve().parents[2] / "tools" / "prepare_tickerhistory.py"
SPEC = importlib.util.spec_from_file_location("prepare_tickerhistory", TOOL)
PREPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARE)
HEADER = b"tradingDate\tsecurityID\tticker_tk\ttodayTicker\topen\thigh\tlow\tclose\tvolume\tshares\tcumulReturnFactor\r\n"
MEMBER = "tbltickerhistory3_10y.txt"
DAY = dt.date(2026, 5, 1)


def row(sid="1", **fields):
    data = dict(tradingDate="2026-05-01", securityID=sid, ticker_tk="OLD", todayTicker="NEW",
                open="10.00", high="12", low="9", close="11.0", volume="100", shares="20",
                cumulReturnFactor="0.5")
    data.update(fields)
    return "\t".join(data[k.decode()] for k in HEADER.rstrip().split(b"\t")).encode() + b"\r\n"


class PreparationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def source(self, rows):
        path = self.root / "source.zip"
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr(MEMBER, HEADER + b"".join(rows))
        return path

    def test_all_duplicate_rows_quarantined_and_exact_original_rows_retained(self):
        first, second = row("12"), row("00012", close="10.5")
        valid = row("13", volume="0", shares="0")
        source = self.source([first, valid, second])
        source_hash = PREPARE.sha256(source)
        manifest = PREPARE.prepare(source, self.root / "out", DAY, DAY)
        self.assertEqual(manifest["counts"]["accepted_rows"], 1)
        self.assertEqual(manifest["counts"]["rejected_rows"], 2)
        self.assertEqual(manifest["nonrejecting_flags"]["accepted_zero_shares"], 1)
        self.assertEqual(manifest["nonrejecting_flags"]["accepted_zero_volume"], 1)
        with zipfile.ZipFile(self.root / "out" / "accepted.zip") as archive:
            self.assertEqual(archive.read(MEMBER), HEADER + valid)
        with zipfile.ZipFile(self.root / "out" / "conflict_fixture.zip") as archive:
            self.assertEqual(archive.read(MEMBER), HEADER + first + second)
        self.assertEqual(PREPARE.sha256(source), source_hash)
        rerun = PREPARE.prepare(source, self.root / "rerun", DAY, DAY)
        self.assertEqual(rerun["accepted"]["sha256"], manifest["accepted"]["sha256"])
        with self.assertRaises(FileExistsError):
            PREPARE.prepare(source, self.root / "out", DAY, DAY)

    def test_numeric_bounds_and_unrepaired_invalid_prices(self):
        cases = [dict(low="-9"), dict(close="13"), dict(open="NaN"),
                 dict(high="1e309"), dict(cumulReturnFactor="0"),
                 dict(cumulReturnFactor="1e308"), dict(cumulReturnFactor="1e-999"),
                 dict(volume="-1"), dict(volume="1_000"), dict(volume="1e-999"), dict(open=" 10"),
                 dict(securityID="0"), dict(securityID="9223372036854775808")]
        rows = []
        for i, fields in enumerate(cases, 1):
            identity = fields.pop("securityID", str(i))
            rows.append(row(identity, **fields))
        valid = row("100", shares="not-reported")
        manifest = PREPARE.prepare(self.source(rows + [valid]), self.root / "out", DAY, DAY)
        self.assertEqual(manifest["counts"]["rejected_rows"], len(cases))
        self.assertEqual(manifest["counts"]["accepted_rows"], 1)
        self.assertEqual(sum(manifest["primary_rejection_counts"].values()), len(cases))
        self.assertEqual(manifest["nonrejecting_flags"]["accepted_invalid_negative_or_out_of_range_shares"], 1)

    def test_bad_width_date_and_order_leave_only_failed_artifacts(self):
        for i, rows in enumerate(([row(), b"bad\twidth\n"],
                                  [row(), row("2", tradingDate="2026-02-30")],
                                  [row(), row("2", tradingDate="2026-04-30")])):
            source = self.source(rows)
            output = self.root / f"failed{i}"
            with self.assertRaises(ValueError):
                PREPARE.prepare(source, output, DAY, DAY)
            self.assertTrue((output / "failure.json").exists())
            self.assertFalse((output / "manifest.json").exists())
            self.assertFalse((output / "accepted.zip").exists())

    def test_shared_predicates_preserve_overlapping_reason_and_flag_order(self):
        values = row("12", low="-1", cumulReturnFactor="0", volume="-1", shares="0").rstrip(b"\r\n").split(b"\t")
        field = {key.decode(): i for i, key in enumerate(HEADER.rstrip().split(b"\t"))}
        reasons, flags = PREPARE.classify_row(values, field, 12, {12})
        self.assertEqual(reasons, ["duplicate_positive_date_security_id", "invalid_or_nonpositive_ohlc",
                                  "invalid_or_nonpositive_cumulative_factor", "invalid_or_negative_volume"])
        self.assertEqual(flags, ["zero_shares"])

    def test_qa_v2_rescues_only_sole_order_violations_on_listed_dates(self):
        day = PREPARE.QA_V2_DATES[0]
        next_day = day + dt.timedelta(days=3)
        corrupt = row("1", tradingDate=day.isoformat(), open="11.5", high="11.2", low="11.1", close="11.0")
        also_bad = row("2", tradingDate=day.isoformat(), open="11.5", high="11.2", low="11.1", volume="-1")
        valid = row("3", tradingDate=day.isoformat())
        later = row("4", tradingDate=next_day.isoformat(), open="11.5", high="11.2", low="11.1")
        source = self.source([corrupt, also_bad, valid, later])
        v1 = PREPARE.prepare(source, self.root / "v1", day, next_day)
        self.assertNotIn("qa_version", v1)
        self.assertEqual(v1["counts"]["accepted_rows"], 1)
        v2 = PREPARE.prepare(source, self.root / "v2", day, next_day, qa_rule="qa-v2")
        self.assertEqual(v2["qa_version"], "v2")
        self.assertEqual(v2["policy_version"], "tickerhistory-qa-v2")
        self.assertFalse(v2["accepted"]["rows_preserved_byte_for_byte"])
        self.assertEqual(v2["accepted"]["unmodified_rows"], 1)
        self.assertTrue(v2["accepted"]["unmodified_rows_preserved_byte_for_byte"])
        self.assertEqual(v2["qa_v2_daily_counts"], {day.isoformat(): {
            "accepted_v1": 1, "accepted_v2_rescued": 1, "accepted": 2}})
        self.assertEqual(v2["counts"]["accepted_rows"], 2)
        self.assertEqual(v2["accepted"]["rows_modified_qa_v2"], 1)
        blanked = corrupt.replace(b"\t11.5\t11.2\t11.1\t", b"\t\t\t\t")
        with zipfile.ZipFile(self.root / "v2" / "accepted.zip") as archive:
            self.assertEqual(archive.read(MEMBER), HEADER + blanked + valid)
        with self.assertRaises(ValueError):
            PREPARE.prepare(source, self.root / "outside", day, next_day, (next_day,))

    def test_qa_v2_allowlist_hash_and_unaffected_bytes_match_legacy(self):
        date_bytes = "".join(d.isoformat() + "\n" for d in PREPARE.QA_V2_DATES).encode("ascii")
        self.assertEqual(len(PREPARE.QA_V2_DATES), 19)
        self.assertEqual(hashlib.sha256(date_bytes).hexdigest(), PREPARE.QA_V2_DATES_SHA256)
        day = dt.date(2013, 5, 1)
        original = row("7", tradingDate=day.isoformat())
        source = self.source([original])
        old = PREPARE.prepare(source, self.root / "old", day, day, qa_rule="legacy-v1")
        new = PREPARE.prepare(source, self.root / "new", day, day, qa_rule="qa-v2")
        self.assertEqual(old["accepted"]["sha256"], new["accepted"]["sha256"])
        self.assertTrue(new["accepted"]["rows_preserved_byte_for_byte"])
        self.assertEqual(new["qa_v2_dates"], [])

    def test_qa_v2_never_rescues_duplicates_or_invalid_close(self):
        day = PREPARE.QA_V2_DATES[0]
        corrupt = dict(tradingDate=day.isoformat(), open="11.5", high="11.2", low="11.1")
        records = [row("1", **corrupt), row("01", **corrupt),
                   row("2", close="0", **corrupt), row("3", cumulReturnFactor="1e308", **corrupt),
                   row("4", tradingDate=day.isoformat())]
        result = PREPARE.prepare(self.source(records), self.root / "out", day, day, qa_rule="qa-v2")
        self.assertEqual(result["counts"]["accepted_rows"], 1)
        self.assertEqual(result["accepted"]["rows_modified_qa_v2"], 0)


if __name__ == "__main__":
    unittest.main()
