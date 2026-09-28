"""Small synthetic postimplementation checks; no real archive/warehouse access."""
import contextlib
import datetime as dt
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import prepare_recent_research as tool


def fixture(path, future=False):
    days = np.busday_offset(np.datetime64("2018-06-01"), np.arange(445)).astype("datetime64[D]")
    date = np.repeat(days, 6)
    ids = np.tile(np.arange(1, 7, dtype=np.int64), len(days))
    raw = np.tile(np.array([100, 90, 80, 70, 60, 50], dtype=np.float32), len(days))
    volume = np.full(len(date), 100000.0)
    # Entire invalid session must remain in the source calendar.
    raw[10 * 6:11 * 6] = np.nan
    keep = ~((date == days[400]) & (ids == 1))
    date, ids, raw, volume = date[keep], ids[keep], raw[keep], volume[keep]
    if future:
        raw[(date >= days[420]) & (ids == 6)] = 50000
    # One positive duplicate key: both numerical rows must be quarantined.
    duplicate = int(np.flatnonzero((date == days[50]) & (ids == 2))[0])
    date = np.concatenate((date, date[[duplicate]], np.array(["2025-02-01"], dtype="datetime64[D]")))
    ids = np.concatenate((ids, ids[[duplicate]], [1]))
    raw = np.concatenate((raw, raw[[duplicate]], [np.nan])).astype(np.float32)
    volume = np.concatenate((volume, volume[[duplicate]], [-1.0]))
    # Source order deliberately not chronological; projection owns sorting.
    order = np.arange(len(date))[::-1]
    table = pa.table({"tradingDate": pa.array(date[order], type=pa.date32()),
        "securityID": ids[order], "close": raw[order], "volume": volume[order],
        "cumulReturnFactor": np.full(len(date), 2.0)})
    pq.write_table(table, path, row_group_size=1000)
    return [str(x) for x in days]


class RecentResearch(unittest.TestCase):
    def test_nonfinite_or_unbounded_deadline_is_refused(self):
        for seconds in (float("nan"), float("inf"), 0, -1, 601):
            with self.assertRaises(ValueError):
                tool.Limits(seconds)

    def test_manifest_bytes_are_charged_before_exclusive_publication(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "manifest.json"
            limits = tool.Limits(30)
            limits.disk_bytes = 1
            with self.assertRaisesRegex(ValueError, "publication exceeds"):
                tool.publish(path, {"status": "complete"}, limits)
            self.assertFalse(path.exists())

    def test_projection_filters_before_qa_preserves_calendar_and_quarantines_duplicates(self):
        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
            base = Path(temp); source = base / "source.parquet"; days = fixture(source)
            m = tool.prepare_cache(source, base / "cache", days[0], "2021-01-01", tool.Limits(30))
            self.assertEqual(m["selected_rows"], 445 * 6)  # one missing + one duplicate
            self.assertEqual(m["rejected_rows"], 8)  # six bad prices + both duplicate rows
            self.assertEqual(len(m["session_days"]), 445)
            values = list(tool.calendar_rows(base / "cache/accepted.parquet", m["session_days"], tool.Limits(30)))
            self.assertEqual(len(values[10][1][0]), 0)
            self.assertNotIn(2, values[50][1][0])
            self.assertEqual(float(values[0][1][3][0]), 200.0)

    def test_lagged_membership_independent_presence_full_warmup_union_and_future_prefix(self):
        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
            base = Path(temp); source = base / "source.parquet"; days = fixture(source)
            tool.prepare_cache(source, base / "cache", days[0], "2021-01-01", tool.Limits(30))
            m = tool.create_role(base / "cache", base / "role", days[0], days[400], "2021-01-01", tool.Limits(30), top_n=2)
            ids = np.fromfile(base / "role/ids.u64", dtype="<u8")
            member = np.fromfile(base / "role/member.u8", dtype="u1").reshape(m["dates"], -1)
            present = np.fromfile(base / "role/present.u8", dtype="u1").reshape(member.shape)
            raw = np.fromfile(base / "role/raw_close.f64", dtype="<f8").reshape(member.shape)
            self.assertFalse(member[:63].any())
            one = int(np.flatnonzero(ids == 1)[0])
            self.assertEqual(member[400, one], 1)
            self.assertEqual(present[400, one], 0)
            self.assertTrue(np.isnan(raw[400, one]))
            self.assertEqual(member[401, one], 0)
            self.assertTrue(np.all(member.sum(axis=1) <= 2))
            self.assertGreaterEqual(m["score_begin"], 383)
            # Changed future eligibility may enlarge the stable union, but cannot
            # alter any earlier historical rank population/member decision.
            other = base / "other.parquet"; fixture(other, True)
            tool.prepare_cache(other, base / "cache2", days[0], "2021-01-01", tool.Limits(30))
            m2 = tool.create_role(base / "cache2", base / "role2", days[0], days[400], "2021-01-01", tool.Limits(30), top_n=2)
            ids2 = np.fromfile(base / "role2/ids.u64", dtype="<u8")
            member2 = np.fromfile(base / "role2/member.u8", dtype="u1").reshape(m2["dates"], -1)
            for t in range(421):
                self.assertEqual(set(ids[member[t] != 0]), set(ids2[member2[t] != 0]))

    def test_unpublished_mutated_cache_budget_and_warmup_refuse(self):
        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
            base = Path(temp); source = base / "source.parquet"; days = fixture(source)
            tool.prepare_cache(source, base / "cache", days[0], "2021-01-01", tool.Limits(30))
            with self.assertRaisesRegex(ValueError, "warmup"):
                tool.create_role(base / "cache", base / "short", days[0], days[380], "2021-01-01", tool.Limits(30), top_n=2)
            with self.assertRaisesRegex(ValueError, "invalid role budget"):
                tool.create_role(base / "cache", base / "budget", days[0], days[400], "2021-01-01", tool.Limits(30), top_n=2, max_output_bytes=16)
            with (base / "cache/accepted.parquet").open("ab") as f:
                f.write(b"x")
            with self.assertRaisesRegex(ValueError, "immutable receipt"):
                tool.cache_receipt(base / "cache", tool.Limits(30))

    def test_between_pass_cache_or_manifest_mutation_never_publishes_role(self):
        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
            base = Path(temp); source = base / "source.parquet"; days = fixture(source)
            for what in ("accepted.parquet", "manifest.json"):
                cache = base / ("cache-" + what)
                tool.prepare_cache(source, cache, days[0], "2021-01-01", tool.Limits(30))
                original_rows = tool.calendar_rows
                def changed_rows(*args, **kwargs):
                    yield from original_rows(*args, **kwargs)
                    with (cache / what).open("ab") as f:
                        f.write(b" ")
                out = base / ("role-" + what)
                with patch.object(tool, "calendar_rows", changed_rows):
                    with self.assertRaisesRegex(ValueError, "changed between role passes"):
                        tool.create_role(cache, out, days[0], days[400], "2021-01-01", tool.Limits(30), top_n=2)
                self.assertFalse((out / "manifest.json").exists())



# ---------------------------------------------------------------- V6-U universe linked-operating-v1
def sha(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def at(day: str, hour=0) -> dt.datetime:
    return dt.datetime.combine(dt.date.fromisoformat(day), dt.time(hour))


def write_listed(directory: Path, name: str, table, schema: str, **extra) -> str:
    directory.mkdir()
    pq.write_table(table, directory / name)
    blob = (directory / name).read_bytes()
    manifest = {"schema": schema, "status": "complete", "files": {name: {"bytes": len(blob), "sha256": sha(blob)}},
                **extra}
    (directory / "manifest.json").write_bytes(json.dumps(manifest).encode())
    return sha((directory / "manifest.json").read_bytes())


BRIDGE_SCHEMA = pa.schema([("sr_id", pa.int64()), ("cik", pa.int64()), ("start", pa.date32()), ("end_incl", pa.date32()),
                           ("available_at", pa.timestamp("us")), ("primary", pa.string()), ("tier", pa.string()),
                           ("basis", pa.string()), ("class_status", pa.string())])
SIC_SCHEMA = pa.schema([("cik", pa.int64()), ("accepted_utc", pa.timestamp("us")), ("accession", pa.string()),
                        ("sic", pa.int64())])


def bridge_rows(days):
    d = lambda i: dt.date.fromisoformat(days[i])  # noqa: E731
    row = lambda sr, cik, start, end, avail, primary="P", status="common": {  # noqa: E731
        "sr_id": sr, "cik": cik, "start": start, "end_incl": end, "available_at": avail, "primary": primary,
        "tier": "high", "basis": "reconstructed_high", "class_status": status}
    return [row(1, 1001, d(0), None, at(days[0])),                       # P, common; SIC visible from t=101
            row(2, 1001, d(0), None, at(days[0]), primary="J"),          # second line of 1001: secondary
            row(3, 1003, d(0), d(199), at(days[0]), status="unknown"),   # class unknown, then common from t=200
            row(3, 1003, d(200), None, at(days[200])),
            row(4, 1004, d(0), None, at(days[0])),                       # SIC 6770: blank check (SPAC)
            row(5, 1005, d(300), None, at(days[310], 23))]               # asserted late: usable from t=311 only


def sic_rows(days, late_filing=False):
    rows = [(1001, at(days[100], 12), "a1", 2834), (1003, at(days[5], 12), "a3", 7372),
            (1004, at(days[5], 12), "a4", 6770), (1005, at(days[5], 12), "a5", 7372), (9999, at(days[5]), "z", 1000)]
    if late_filing:  # a later filing re-codes 1004 as operating: it may change sessions after it, never before
        rows.append((1004, at(days[430], 12), "a4b", 2834))
    return pa.Table.from_pylist([dict(zip(SIC_SCHEMA.names, r)) for r in rows], schema=SIC_SCHEMA)


class LinkedOperatingUniverse(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)
        self._quiet = contextlib.redirect_stdout(io.StringIO())
        self._quiet.__enter__()
        source = self.base / "source.parquet"
        self.days = fixture(source)
        tool.prepare_cache(source, self.base / "cache", self.days[0], "2021-01-01", tool.Limits(30))
        tool.create_role(self.base / "cache", self.base / "role", self.days[0], self.days[400], "2021-01-01",
                         tool.Limits(30), top_n=5)
        self.role_sha = sha((self.base / "role/manifest.json").read_bytes())
        self.bridge_sha = write_listed(self.base / "bridge", "links.parquet",
                                       pa.Table.from_pylist(bridge_rows(self.days), schema=BRIDGE_SCHEMA),
                                       "atx.identity-bridge/v1", rehearsal_identity=True)
        self.sic_sha = write_listed(self.base / "events", "sic_events.parquet", sic_rows(self.days),
                                    "atx.fundamental-events/v1")

    def tearDown(self):
        self._quiet.__exit__(None, None, None)
        self._tmp.cleanup()

    def restrict(self, out="lo", **kw):
        args = dict(bridge=self.base / "bridge", bridge_sha256=self.bridge_sha, sic_events=self.base / "events",
                    sic_events_sha256=self.sic_sha)
        args.update(kw)
        return tool.restrict_role(self.base / "role", self.role_sha, self.base / out, tool.Limits(60), **args)

    def arrays(self, directory):
        m = json.loads((self.base / directory / "manifest.json").read_bytes())
        ids = np.fromfile(self.base / directory / "ids.u64", dtype="<u8")
        member = np.fromfile(self.base / directory / "member.u8", dtype="u1").reshape(m["dates"], -1)
        return m, ids, member

    def expected(self, base_member, ids):
        """The rule by hand for this fixture (member days only): ids 2 and 4 never, 1 from t=101 (SIC lag 1),
        3 from t=200 (class), 5 from t=311 (late-asserted link), SIC older than 550 days drops."""
        day = lambda t: dt.date.fromisoformat(self.days[t])  # noqa: E731
        fresh = lambda t, filed: (day(t) - day(filed)).days <= 550  # noqa: E731
        col = {int(x): k for k, x in enumerate(ids)}
        want = np.zeros_like(base_member)
        for t in range(base_member.shape[0]):
            for sid, ok in ((1, t >= 101 and fresh(t, 100)), (3, t >= 200 and fresh(t, 5)),
                            (5, t >= 311 and fresh(t, 5))):
                if sid in col and base_member[t, col[sid]] and ok:
                    want[t, col[sid]] = 1
        return want

    def test_classifier_reasons_and_precedence(self):
        member = [True] * 7 + [False]
        link = [-1, -2, 3, 3, 3, 3, 3, 3]
        primary = [False, False, False, True, True, True, True, True]
        not_common = [False, False, False, True, False, False, False, False]
        visible = [False, False, False, True, False, True, True, True]
        code = [0, 0, 0, 2834, 0, 6770, 2834, 2834]
        keep, reason = tool.classify_linked_operating(member, link, primary, not_common, visible, code)
        self.assertEqual(reason.tolist(), [1, 2, 3, 4, 5, 6, 0, -1])
        self.assertEqual(keep.tolist(), [False] * 6 + [True, False])
        self.assertEqual(tool.UNIVERSE_REASONS, ("unlinked", "ambiguous", "secondary_line", "class_not_common",
                                                 "no_visible_sic", "non_operating_sic"))
        # pooled vehicles, blank checks and royalty trusts (6792 / 6795, controller ruling V6-W fix round 1);
        # REITs (6798) stay
        self.assertEqual(tool.NON_OPERATING_SIC, (6189, 6221, 6722, 6726, 6770, 6792, 6795))
        for sic in (6189, 6221, 6722, 6726, 6770, 6792, 6795):
            self.assertEqual(tool.classify_linked_operating([True], [0], [True], [False], [True], [sic])[1][0], 6)
        self.assertEqual(tool.classify_linked_operating([True], [0], [True], [False], [True], [6798])[1][0], 0)
        # first failing test wins: an unlinked line that also lacks SIC is 'unlinked'
        self.assertEqual(tool.classify_linked_operating([True], [-1], [False], [True], [False], [6770])[1][0], 1)

    def test_restricts_members_point_in_time_and_copies_payloads(self):
        m = self.restrict()
        base_m, ids, base_member = self.arrays("role")
        _, ids2, member = self.arrays("lo")
        np.testing.assert_array_equal(ids, ids2)
        want = self.expected(base_member, ids)
        np.testing.assert_array_equal(member, want)
        col = {int(x): k for k, x in enumerate(ids)}
        self.assertEqual((member[100, col[1]], member[101, col[1]]), (0, 1))   # SIC accepted d100 12:00: t=101
        self.assertEqual((member[310, col[5]], member[311, col[5]]), (0, 1))   # link asserted d310 23:00: t=311
        self.assertEqual((member[199, col[3]], member[200, col[3]]), (0, 1))   # class common from its own row
        self.assertFalse(member[:, col[2]].any() or member[:, col[4]].any())
        self.assertTrue((member <= base_member).all())
        for name in ("close.f64", "raw_close.f64", "volume.f64", "present.u8", "sessions.i64", "ids.u64"):
            self.assertEqual((self.base / "lo" / name).read_bytes(), (self.base / "role" / name).read_bytes(), name)
            self.assertEqual(m["files"][name], base_m["files"][name])
        self.assertEqual(m["files"]["member.u8"]["sha256"], sha((self.base / "lo/member.u8").read_bytes()))
        for key in ("membership_recipe", "common_stock_verified", "dates", "instruments", "score_begin",
                    "declared_output_bytes", "source_sha256", "clock_recipe"):
            self.assertEqual(m[key], base_m[key], key)
        self.assertEqual(m["score_member_counts"],
                         [int(x) for x in member.astype(np.int64).sum(axis=1)[m["score_begin"]:]])
        u = m["universe"]
        self.assertEqual((u["id"], u["class_status_required"], u["sic_lag_sessions"], u["sic_stale_days"]),
                         ("linked-operating-v1", "common", 1, 550))
        self.assertEqual(u["base_role"]["manifest_sha256"], self.role_sha)
        # member.u8 arrays are uint8: every count is taken in int64 (numpy 2 NEP 50 keeps uint8 scalar sums uint8).
        wide_base, wide = base_member.astype(np.int64), member.astype(np.int64)
        base_counts, kept_counts = wide_base.sum(axis=1), wide.sum(axis=1)
        self.assertEqual(u["base_member_counts"], base_counts.tolist())
        self.assertEqual(u["kept_member_counts"], kept_counts.tolist())
        self.assertEqual(u["dropped_member_share"], [round(1 - k / b, 6) if b else None
                                                    for b, k in zip(base_counts, kept_counts)])
        self.assertIsNone(u["dropped_member_share"][0])
        self.assertEqual(sum(u["dropped_by_reason"].values()), int(wide_base.sum() - wide.sum()))
        self.assertEqual(u["dropped_by_reason"]["secondary_line"], int(wide_base[:, col[2]].sum()))
        stale = [(dt.date.fromisoformat(self.days[t]) - dt.date.fromisoformat(self.days[5])).days > 550
                 for t in range(base_member.shape[0])]  # 1004's only SIC row (d5) ages out: no_visible_sic
        self.assertTrue(any(stale))
        self.assertEqual(u["dropped_by_reason"]["non_operating_sic"],
                         int(wide_base[[t for t, s in enumerate(stale) if not s], col[4]].sum()))
        self.assertEqual(u["dropped_by_reason"]["unlinked"], int(wide_base[:311, col[5]].sum()))
        self.assertEqual(u["link_member_cells"]["member_cells"], int(wide_base.sum()))
        self.assertEqual(u["inputs"]["identity_bridge"]["class_status_rows"], {"common": 5, "unknown": 1})
        self.assertEqual(u["inputs"]["identity_bridge"]["checks"]["rows_used_available_after_start_mark"], 1)
        self.assertIsNone(u["fields_crosscheck"])

    def test_later_filing_never_changes_earlier_sessions(self):
        self.restrict("first")
        sha2 = write_listed(self.base / "events2", "sic_events.parquet", sic_rows(self.days, late_filing=True),
                            "atx.fundamental-events/v1")
        self.restrict("second", sic_events=self.base / "events2", sic_events_sha256=sha2)
        _, ids, a = self.arrays("first")
        _, _, b = self.arrays("second")
        col = {int(x): k for k, x in enumerate(ids)}
        np.testing.assert_array_equal(a[:431], b[:431])  # filed d430 12:00 -> visible from t=431 only
        self.assertTrue(b[431:, col[4]].all() and not a[:, col[4]].any())

    def test_fields_crosscheck_and_refusals(self):
        m = self.restrict("plain")
        base_m, ids, _ = self.arrays("role")
        nd, n = base_m["dates"], base_m["instruments"]
        day = lambda t: dt.date.fromisoformat(self.days[t])  # noqa: E731
        col = {int(x): k for k, x in enumerate(ids)}
        grid = np.full((nd, n), np.nan)  # finite(grp_ff12) = primary-linked & visible SIC, members or not
        for t in range(nd):
            for sid, first, filed in ((1, 101, 100), (3, 6, 5), (4, 6, 5), (5, 311, 5)):
                if t >= first and (day(t) - day(filed)).days <= 550:
                    grid[t, col[sid]] = 11.0
        fields = self.base / "fields"
        fields.mkdir()

        def write_fields(values, link_counts):
            (fields / "grp_ff12.f64").write_bytes(values.astype("<f8").tobytes())
            blob = (fields / "grp_ff12.f64").read_bytes()
            doc = {"schema": "atx.research-role-fields/v1", "status": "complete",
                   "role": {"manifest_sha256": self.role_sha},
                   "source_checks": {"issuer": {"fund_lag_sessions": 1, "link_member_cells": link_counts}},
                   "fields": [{"name": "grp_ff12", "file": "grp_ff12.f64", "sha256": sha(blob), "shape": [nd, n]}]}
            (fields / "manifest.json").write_bytes(json.dumps(doc).encode())
            return sha((fields / "manifest.json").read_bytes())

        fsha = write_fields(grid, m["universe"]["link_member_cells"])
        checked = self.restrict("checked", fields=fields, fields_sha256=fsha)
        self.assertEqual(checked["universe"]["fields_crosscheck"]["grp_ff12_finite_equals_linked_primary_visible_sic_cells"],
                         nd * n)
        self.assertEqual((self.base / "checked/member.u8").read_bytes(), (self.base / "plain/member.u8").read_bytes())
        bad = grid.copy()
        bad[400, col[2]] = 11.0  # a secondary line with a group label: not the fields' link rule
        with self.assertRaisesRegex(ValueError, "grp_ff12"):
            self.restrict("bad_grid", fields=fields, fields_sha256=write_fields(bad, m["universe"]["link_member_cells"]))
        counts = dict(m["universe"]["link_member_cells"], unlinked=0)
        with self.assertRaisesRegex(ValueError, "link_member_cells"):
            self.restrict("bad_counts", fields=fields, fields_sha256=write_fields(grid, counts))
        for out in ("bad_grid", "bad_counts"):
            self.assertFalse((self.base / out).exists())  # refused before the output directory exists
        with self.assertRaisesRegex(ValueError, "does not match"):
            tool.restrict_role(self.base / "role", "0" * 64, self.base / "pin", tool.Limits(60),
                               bridge=self.base / "bridge", bridge_sha256=self.bridge_sha,
                               sic_events=self.base / "events", sic_events_sha256=self.sic_sha)
        with self.assertRaisesRegex(ValueError, "identity-bridge"):
            self.restrict("bridge_pin", bridge_sha256="0" * 64)
        with self.assertRaises(FileExistsError):
            self.restrict("plain")  # outputs are never overwritten
        with self.assertRaisesRegex(ValueError, "already carries"):
            tool.restrict_role(self.base / "plain", sha((self.base / "plain/manifest.json").read_bytes()),
                               self.base / "twice", tool.Limits(60), bridge=self.base / "bridge",
                               bridge_sha256=self.bridge_sha, sic_events=self.base / "events",
                               sic_events_sha256=self.sic_sha)

    def test_cli_default_universe_is_the_current_rule(self):
        self.assertEqual(tool.DEFAULT_UNIVERSE, "research-prior63-usd-adv-topn-v1")
        self.assertEqual(json.loads(json.loads((self.base / "role/manifest.json").read_bytes())["membership_recipe"])
                         ["rule"], tool.DEFAULT_UNIVERSE)
        self.assertNotIn("universe", json.loads((self.base / "role/manifest.json").read_bytes()))
        for argv in (["role", "--out", str(self.base / "x"), "--universe", "linked-operating-v1",
                      "--cache", str(self.base / "cache")],
                     ["role", "--out", str(self.base / "x"), "--cache", str(self.base / "cache"),
                      "--base-role", str(self.base / "role")],
                     ["role", "--out", str(self.base / "x"), "--universe", "linked-operating-v1",
                      "--base-role", str(self.base / "role")]):
            with patch.object(sys, "argv", ["prepare_recent_research.py", *argv]), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                tool.main()
        self.assertFalse((self.base / "x").exists())

if __name__ == "__main__":
    unittest.main()
