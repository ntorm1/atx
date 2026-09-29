"""linked-operating-v3 (platform v7 U2): v1 / v2 output byte identity, the atx-db fundamentals-stage SIC adapter (column
mapping, the unchanged lag-1 / 550-day clock, carried rows, the seal), v3 membership against v2, and the CLI; on the
synthetic role of test_prepare_recent_research / test_linked_operating_v2."""
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
import prepare_research_fields as prf
import test_linked_operating_v2 as v2t
import test_prepare_recent_research as base

# Normalised v2 outputs of LinkedOperatingV2's fixture computed with prepare_recent_research.py at the U2 base commit
# a5e4a10d (before linked-operating-v3 existed): pins and paths replaced by tokens, universe.inputs.code removed.
V2_GOLDEN = {"plain": ("3757fb927b5610af6172db5323910bba5bf4ee4a65838360d705fa47aa1eca9b",
                       "d1e3c2f39e3d9c2d81bace104ecafc973ab04e4577e747aa2a6f0813b837bfc1"),
             "returns": ("855d37301b1d28fc422f17931f03697ec39bb5398500a047e64b787bc25a15b4",
                         "a5300162bfffa2c94cc57d9cf6bb0ca7165328219d37b5daef124bccc41ec37a")}
V2_GOLDEN_RETURNS_CLOSE = "1a403d1f8ec19192ba56b6328f1022f7811c3192ba8dd7eeb3b10b3daf8dc57b"
STAGE_SCHEMA = pa.schema([("cik", pa.int64()), ("clock_utc", pa.timestamp("us")), ("sic", pa.int32()),
                          ("sic2", pa.int32()), ("ff12", pa.string()), ("ff49", pa.string()),
                          ("accession", pa.string()), ("sic_basis", pa.string())])


def normalized(case, m, out):
    root = case.base
    tokens = {case.role_sha: "<role-sha>", case.bridge_sha: "<bridge-sha>", case.sic_sha: "<sic-sha>",
              case.del_sha: "<delisting-sha>",
              base.sha((root / "bridge/links.parquet").read_bytes()): "<bridge-file-sha>",
              base.sha((root / "events/sic_events.parquet").read_bytes()): "<sic-file-sha>",
              base.sha((root / "delisting/events.parquet").read_bytes()): "<delisting-file-sha>",
              base.sha((root / "source.parquet").read_bytes()): "<source-sha>",
              base.sha((root / "cache/manifest.json").read_bytes()): "<projection-sha>"}
    roots = sorted({str(root), str(root.resolve())}, key=len, reverse=True)
    doc = json.loads(json.dumps(m))
    doc["universe"]["inputs"].pop("code")

    def walk(x):
        if isinstance(x, dict):
            return {k: walk(v) for k, v in x.items()}
        if isinstance(x, list):
            return [walk(v) for v in x]
        if isinstance(x, str):
            for r in roots:
                x = x.replace(r, "<BASE>")
            return tokens.get(x, x)
        return x
    blob = json.dumps(walk(doc), sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(blob).hexdigest(), base.sha((root / out / "member.u8").read_bytes())


class V1V2Identity(unittest.TestCase):
    def test_v1_output_is_byte_identical_to_the_base_commit(self):
        case = base.LinkedOperatingUniverse("test_restricts_members_point_in_time_and_copies_payloads")
        case.setUp()
        try:
            m = case.restrict()
            self.assertEqual(v2t.normalized_v1(case, m), (v2t.V1_GOLDEN_MANIFEST, v2t.V1_GOLDEN_MEMBER))
        finally:
            case.tearDown()

    def test_v2_outputs_are_byte_identical_to_the_base_commit(self):
        case = v2t.LinkedOperatingV2("test_class_rule")
        case.setUp()
        try:
            plain = case.restrict("v2")
            ret = case.restrict("ret", delisting_returns=True)
            self.assertEqual(normalized(case, plain, "v2"), V2_GOLDEN["plain"])
            self.assertEqual(normalized(case, ret, "ret"), V2_GOLDEN["returns"])
            self.assertEqual(base.sha((case.base / "ret" / "close.f64").read_bytes()), V2_GOLDEN_RETURNS_CLOSE)
        finally:
            case.tearDown()


def stage_table(rows, wrong_label=()):
    """Stage rows (cik, clock_utc naive UTC, sic, accession, sic_basis) with the stage's labels (atx-db
    reference_classifications, as its _ff_lookup); accessions in ``wrong_label`` get a wrong FF12 label."""
    rc = prf.reference_classifications()
    out = []
    for cik, clock, sic, acc, basis in rows:
        ok = sic is not None and 100 <= sic <= 9999
        out.append({"cik": cik, "clock_utc": clock, "sic": sic, "sic2": None if sic is None else sic // 100,
                    "ff12": ("Money" if acc in wrong_label else rc.fama_french_12_for_sic(sic)) if ok else "Other",
                    "ff49": rc.fama_french_49_for_sic(sic) if ok else None, "accession": acc, "sic_basis": basis})
    return pa.Table.from_pylist(out, schema=STAGE_SCHEMA)


def write_stage(directory, rows, wrong_label=(), **extra):
    extra = {"stage": "fundamentals", "rule": "fund-events-pit-v2", "code_version": "fundamentals-v9", **extra}
    return base.write_listed(directory, "sic_events.parquet", stage_table(rows, wrong_label),
                             "atx.alpha-panel.fundamentals/v2", **extra)


def events_table(rows):
    """The same rows as an atx.fundamental-events/v1 SIC table (clock_utc -> accepted_utc; sic_basis dropped)."""
    return pa.Table.from_pylist([dict(zip(base.SIC_SCHEMA.names, (c, t, a, s))) for c, t, s, a, _ in rows],
                                schema=base.SIC_SCHEMA)


def stage_rows(days, carried=True):
    at = base.at
    rows = [(1001, at(days[100], 22), 2834, "a1", "fsds_sub"),     # clock == the d100 mark: visible from t=102
            (1003, at(days[5], 12), 7372, "a3", "fsds_sub"),
            (1003, at(days[20], 12), 50, "a3x", "fsds_sub"),       # SIC outside [100, 9999]: skipped, 7372 stays
            (1004, at(days[5], 12), 2834, "a4", "fsds_sub"),       # operating here; 550 days old near the end ...
            (1005, at(days[5], 12), 7372, "a5", "fsds_sub"),
            (1001, dt.datetime(2025, 2, 1), 1000, "a1s", "fsds_sub"),   # clock after the seal: dropped
            (9999, at(days[5]), 1000, "z", "fsds_sub")]              # no linked line
    if carried:  # ... unless a later filing carries the SIC in force to its own clock
        rows.append((1004, at(days[300], 12), 2834, "a4c", "carried"))
    return rows


class LinkedOperatingV3(unittest.TestCase):
    def setUp(self):
        self.v2 = v2t.LinkedOperatingV2("test_class_rule")
        self.v2.setUp()
        self.base, self.days = self.v2.base, self.v2.days
        self.stage_sha = write_stage(self.base / "stage", stage_rows(self.days), wrong_label=("a5",))

    def tearDown(self):
        self.v2.tearDown()

    def restrict(self, out, universe="linked-operating-v3", **kw):
        if universe == "linked-operating-v3":
            kw.setdefault("sic_events", self.base / "stage")
            kw.setdefault("sic_events_sha256", self.stage_sha)
            kw.setdefault("delisting", self.base / "delisting")
            kw.setdefault("delisting_sha256", self.v2.del_sha)
        return self.v2.restrict(out, universe=universe, **kw)

    def member(self, out):
        m = json.loads((self.base / out / "manifest.json").read_bytes())
        return np.fromfile(self.base / out / "member.u8", dtype="u1").reshape(m["dates"], -1)

    def oracle(self, rows):
        """The v3 rule by hand for this fixture: v2 links and classes (lines 1, 3, 4 pass; 2 is J, 5 a unit), the
        linked CIK's latest valid unsealed row with clock < the t-1 mark (ties by accession), age <= 550 days."""
        days = [dt.date.fromisoformat(d) for d in self.days]
        mark = lambda t: dt.datetime.combine(days[t], dt.time(22))  # noqa: E731
        valid = [r for r in rows if 100 <= r[2] <= 9999 and r[1] < dt.datetime(2025, 1, 1)]
        role = self.v2.arrays("role")
        col = {int(x): k for k, x in enumerate(role["ids"])}
        want = np.zeros_like(role["member.u8"])
        for t in range(1, len(days)):
            for sid, cik in ((1, 1001), (3, 1003), (4, 1004)):
                seen = [r for r in valid if r[0] == cik and r[1] < mark(t - 1)]
                if not seen or sid not in col or not role["member.u8"][t, col[sid]]:
                    continue
                last = max(seen, key=lambda r: (r[1], r[3]))
                if (days[t] - last[1].date()).days <= 550 and last[2] not in tool.NON_OPERATING_SIC:
                    want[t, col[sid]] = 1
        return want, col

    def test_v3_equals_v2_on_the_same_rows(self):
        """The column mapping: v3 over the stage == v2 over the same rows as an atx.fundamental-events/v1 table."""
        rows = stage_rows(self.days)
        sha = base.write_listed(self.base / "events-same", "sic_events.parquet", events_table(rows),
                                "atx.fundamental-events/v1")
        v3 = self.restrict("v3")
        v2 = self.restrict("v2same", universe="linked-operating-v2", sic_events=self.base / "events-same",
                           sic_events_sha256=sha)
        self.assertEqual((self.base / "v3/member.u8").read_bytes(), (self.base / "v2same/member.u8").read_bytes())
        for name in ("close.f64", "raw_close.f64", "volume.f64", "present.u8", "sessions.i64", "ids.u64"):
            self.assertEqual((self.base / "v3" / name).read_bytes(), (self.base / "role" / name).read_bytes(), name)
        a, b = v3["universe"], v2["universe"]
        for key in ("dropped_by_reason", "dropped_by_reason_score_window", "kept_member_counts", "link_member_cells",
                    "class_status_required", "sic_lag_sessions", "sic_stale_days", "delisting"):
            self.assertEqual(a[key], b[key], key)
        self.assertEqual((a["id"], a["rule"], a["limits"]),
                         ("linked-operating-v3", tool.LINKED_OPERATING_V3_RULE, tool.UNIVERSE_V3_LIMITS))
        s = a["inputs"]["sic_events"]
        self.assertEqual((s["manifest_sha256"], s["adapter"], s["column_mapping"]),
                         (self.stage_sha, "atx.alpha-panel.fundamentals/v2", prf.SIC_STAGE_MAPPING))
        self.assertEqual([Path(x["path"]).name for x in s["sources"]], ["manifest.json", "sic_events.parquet"])
        self.assertEqual(s["sources"][0]["sha256"], self.stage_sha)
        for k in ("rows_total", "rows_available_on_or_after_2025_dropped", "rows_ignored_unlinked_cik", "rows_used",
                  "rows_used_invalid_sic_skipped", "rows_sharing_cik_and_clock"):
            self.assertEqual(s["checks"][k], b["inputs"]["sic_events"]["checks"][k], k)
        self.assertEqual((s["checks"]["rows_total"], s["checks"]["rows_available_on_or_after_2025_dropped"],
                          s["checks"]["rows_ignored_unlinked_cik"], s["checks"]["rows_used_invalid_sic_skipped"],
                          s["checks"]["rows_used"]), (8, 1, 1, 1, 5))
        self.assertEqual((s["checks"]["rows_by_sic_basis"], s["checks"]["rows_used_carried"],
                          s["checks"]["rows_stage_labels_differ_from_builder_mapping"]),
                         ({"fsds_sub": 7, "carried": 1}, 1, 1))
        self.assertEqual((s["checks"]["stage"], s["checks"]["stage_rule"]), ("fundamentals", "fund-events-pit-v2"))

    def test_clock_strict_mark_staleness_and_carried_rows(self):
        self.restrict("v3")
        got = self.member("v3")
        want, col = self.oracle(stage_rows(self.days))
        np.testing.assert_array_equal(got, want)
        self.assertEqual((got[101, col[1]], got[102, col[1]]), (0, 1))   # clock == mark(100): strict <, lag 1
        days = [dt.date.fromisoformat(d) for d in self.days]
        late = [t for t in range(len(days)) if (days[t] - days[5]).days > 550]
        self.assertTrue(late and got[late, col[4]].all())                 # the carried row re-dates the age
        sha = write_stage(self.base / "stage-nc", stage_rows(self.days, carried=False))
        m = self.restrict("v3nc", sic_events=self.base / "stage-nc", sic_events_sha256=sha)
        nc = self.member("v3nc")
        np.testing.assert_array_equal(nc, self.oracle(stage_rows(self.days, carried=False))[0])
        self.assertFalse(nc[late, col[4]].any())                          # 550 days from the fsds row: stale
        base_member = self.v2.arrays("role")["member.u8"][:, col[4]] != 0
        fresh = [t for t in range(6, len(days)) if (days[t] - days[5]).days <= 550 and base_member[t]]
        self.assertTrue(fresh and nc[fresh, col[4]].all())
        self.assertEqual(m["universe"]["dropped_by_reason"]["no_visible_sic"] - json.loads(
            (self.base / "v3/manifest.json").read_bytes())["universe"]["dropped_by_reason"]["no_visible_sic"],
            len(late))
        # a later row never changes an earlier session
        np.testing.assert_array_equal(nc[:301], got[:301])

    def test_v3_recovers_cells_the_events_table_lacks(self):
        """W5b-F1 in miniature: the events table lacks CIK 1003 (v2: no_visible_sic), the stage has it (v3: kept)."""
        rows = [r for r in stage_rows(self.days) if r[0] != 1003]
        sha = base.write_listed(self.base / "events-no1003", "sic_events.parquet", events_table(rows),
                                "atx.fundamental-events/v1")
        v2 = self.restrict("v2", universe="linked-operating-v2", sic_events=self.base / "events-no1003",
                           sic_events_sha256=sha)
        v3 = self.restrict("v3")
        a, b = self.member("v2"), self.member("v3")
        col = self.oracle(stage_rows(self.days))[1]
        self.assertFalse(a[:, col[3]].any())
        self.assertTrue(b[:, col[3]].any())
        np.testing.assert_array_equal(np.delete(a, col[3], axis=1), np.delete(b, col[3], axis=1))
        gained = int(b[:, col[3]].astype(np.int64).sum())
        self.assertEqual(v2["universe"]["dropped_by_reason"]["no_visible_sic"]
                         - v3["universe"]["dropped_by_reason"]["no_visible_sic"], gained)

    def test_refusals(self):
        cases = [("bad-pin", {"sic_events_sha256": "0" * 64}, "sic-events manifest SHA-256"),
                 ("v1-table", {"sic_events": self.base / "events", "sic_events_sha256": self.v2.sic_sha},
                  "not a complete atx.alpha-panel.fundamentals/v2"),
                 ("no-delisting", {"delisting": None, "delisting_sha256": None}, "--delisting")]
        sha = write_stage(self.base / "other-stage", stage_rows(self.days), stage="prices")
        cases.append(("other-stage", {"sic_events": self.base / "other-stage", "sic_events_sha256": sha},
                      "stage 'prices' is not 'fundamentals'"))
        rows = stage_rows(self.days) + [(1001, base.at(self.days[9]), 2834, "q", "guessed")]
        sha = write_stage(self.base / "odd-basis", rows)
        cases.append(("odd-basis", {"sic_events": self.base / "odd-basis", "sic_events_sha256": sha}, "sic_basis"))
        tampered = self.base / "tampered"
        sha = write_stage(tampered, stage_rows(self.days))
        (tampered / "sic_events.parquet").write_bytes((tampered / "sic_events.parquet").read_bytes() + b"\0")
        cases.append(("tampered", {"sic_events": tampered, "sic_events_sha256": sha}, "does not match its manifest"))
        for out, kw, pattern in cases:
            with self.assertRaisesRegex(ValueError, pattern):
                self.restrict("out-" + out, **kw)
            self.assertFalse((self.base / ("out-" + out)).exists(), out)

    def test_fields_crosscheck_requires_the_same_sic_stage(self):
        m = self.restrict("plain")
        role = self.v2.arrays("role")
        nd, n = role["member.u8"].shape
        want, col = self.oracle(stage_rows(self.days))
        days = [dt.date.fromisoformat(d) for d in self.days]
        mark = lambda t: dt.datetime.combine(days[t], dt.time(22))  # noqa: E731
        grid = np.full((nd, n), np.nan)   # finite(grp_ff12) = primary-linked & visible SIC on every cell
        valid = [r for r in stage_rows(self.days) if 100 <= r[2] <= 9999 and r[1] < dt.datetime(2025, 1, 1)]
        for t in range(1, nd):
            for sid, cik, first in ((1, 1001, 0), (3, 1003, 0), (4, 1004, 0), (5, 1005, 311)):
                seen = [r for r in valid if r[0] == cik and r[1] < mark(t - 1)]
                if t >= first and seen and (days[t] - max(seen, key=lambda r: (r[1], r[3]))[1].date()).days <= 550:
                    grid[t, col.get(sid, 0)] = 10.0
        fields = self.base / "fields"
        fields.mkdir()

        def write_fields(source_sha):
            (fields / "grp_ff12.f64").write_bytes(grid.astype("<f8").tobytes())
            blob = (fields / "grp_ff12.f64").read_bytes()
            doc = {"schema": "atx.research-role-fields/v1", "status": "complete",
                   "role": {"manifest_sha256": self.v2.role_sha},
                   "source_checks": {"issuer": {"fund_lag_sessions": 1,
                                                "link_member_cells": m["universe"]["link_member_cells"]}},
                   "fields": [{"name": "grp_ff12", "file": "grp_ff12.f64", "sha256": base.sha(blob), "shape": [nd, n],
                               "sources": [{"path": "x/manifest.json", "sha256": source_sha}]}]}
            (fields / "manifest.json").write_bytes(json.dumps(doc).encode())
            return base.sha((fields / "manifest.json").read_bytes())

        with self.assertRaisesRegex(ValueError, "not built from the --sic-events stage"):
            self.restrict("other-table", fields=fields, fields_sha256=write_fields(self.v2.sic_sha))
        self.assertFalse((self.base / "other-table").exists())
        checked = self.restrict("checked", fields=fields, fields_sha256=write_fields(self.stage_sha))
        c = checked["universe"]["fields_crosscheck"]
        self.assertEqual((c["grp_ff12_sic_events_manifest_sha256"],
                          c["grp_ff12_finite_equals_linked_primary_visible_sic_cells"]), (self.stage_sha, nd * n))
        self.assertEqual((self.base / "checked/member.u8").read_bytes(), (self.base / "plain/member.u8").read_bytes())

    def test_cli(self):
        common = ["role", "--base-role", str(self.base / "role"), "--base-role-sha256", self.v2.role_sha,
                  "--identity-bridge", str(self.base / "bridge"), "--identity-bridge-sha256", self.v2.bridge_sha,
                  "--sic-events", str(self.base / "stage" / "sic_events.parquet"), "--sic-events-sha256",
                  self.stage_sha, "--universe", "linked-operating-v3"]
        with patch.object(sys, "argv", ["prepare_recent_research.py", *common, "--out", str(self.base / "x")]), \
                contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            tool.main()   # v3 needs the delisting stage, as v2
        self.assertFalse((self.base / "x").exists())
        argv = common + ["--out", str(self.base / "cli"), "--delisting", str(self.base / "delisting"),
                         "--delisting-sha256", self.v2.del_sha]
        with patch.object(sys, "argv", ["prepare_recent_research.py", *argv]):
            tool.main()
        self.restrict("api")
        self.assertEqual((self.base / "cli/member.u8").read_bytes(), (self.base / "api/member.u8").read_bytes())
        m = json.loads((self.base / "cli/manifest.json").read_bytes())
        self.assertEqual(m["universe"]["id"], "linked-operating-v3")
        self.assertEqual(Path(m["universe"]["inputs"]["sic_events"]["sources"][0]["path"]).parent.name, "stage")


if __name__ == "__main__":
    unittest.main()
