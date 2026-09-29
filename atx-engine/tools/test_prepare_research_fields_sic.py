"""--sic-events (platform v7 U2): the grp_* fields read the atx-db fundamentals stage's SIC table, the table role
universe linked-operating-v3 restricts on. Without the option every payload is the base commit's; with a stage holding
the same rows every payload (grp_* included) is byte-identical; with different rows only grp_* cells move, exactly where
the two tables disagree; --reuse never crosses SIC tables. Synthetic fixture of test_prepare_research_fields."""
import contextlib
import datetime as dt
import hashlib
import io
import json
import math
from pathlib import Path
import tempfile
import unittest

import numpy as np
import pyarrow as pa

import prepare_research_fields as tool
import test_prepare_research_fields as T

# SHA-256 of the canonical files dict (payload pins) of the ISSUER_RUN build below, computed with
# prepare_research_fields.py at the U2 base commit a5e4a10d (before --sic-events existed).
GOLDEN_ISSUER_FILES = "784e0496f18fde9d2b18cd5e5e7f73d8aaa19b5ee69488706ccfeee7ece56566"
GRP = ("grp_sic2", "grp_ff12", "grp_ff49")
STAGE_SCHEMA = pa.schema([("cik", pa.int64()), ("clock_utc", pa.timestamp("us")), ("sic", pa.int32()),
                          ("sic2", pa.int32()), ("ff12", pa.string()), ("ff49", pa.string()),
                          ("accession", pa.string()), ("sic_basis", pa.string())])
# (cik, clock, sic, accession, sic_basis): the events fixture's SIC rows, as the stage publishes them
SAME = [(c, t, s, a, "fsds_sub") for c, t, s, a in T.SIC_EVENTS]
DIFF = [(c, t, 2834 if a == "0000001001-24-000005" else s, a, "fsds_sub") for c, t, s, a in T.SIC_EVENTS] + [
    (2002, T.at(2024, 10, 20, 12), 6022, "0000002002-24-000009", "carried")]   # re-dates 2002's 2023 SIC


def files_digest(manifest) -> str:
    return hashlib.sha256(json.dumps(manifest["files"], sort_keys=True).encode()).hexdigest()


def write_stage(root, rows, **flags):
    rc = tool.reference_classifications()
    out = []
    for cik, clock, sic, acc, basis in rows:
        ok = sic is not None and 100 <= sic <= 9999
        out.append({"cik": cik, "clock_utc": clock.replace(tzinfo=None), "sic": sic,
                    "sic2": None if sic is None else sic // 100, "ff12": rc.fama_french_12_for_sic(sic) if ok else "Other",
                    "ff49": rc.fama_french_49_for_sic(sic) if ok else None, "accession": acc, "sic_basis": basis})
    flags = {"stage": "fundamentals", "rule": "fund-events-pit-v2", "code_version": "fundamentals-v9", **flags}
    return T.write_published(root, "atx.alpha-panel.fundamentals/v2",
                             {"sic_events.parquet": pa.Table.from_pylist(out, schema=STAGE_SCHEMA)}, **flags)


def oracle_grp(rows, kind, lag=1):
    """test_prepare_research_fields.oracle_grp over ``rows`` (latest valid unsealed row before the t-lag mark, ties by
    accession, 550-day age, primary lines only)."""
    valid = [r for r in rows if r[2] is not None and 100 <= r[2] <= 9999]
    out = np.full((len(T.SESSIONS), len(T.IDS)), np.nan)
    for t, d in enumerate(T.SESSIONS):
        for i, sid in enumerate(T.IDS):
            cik, primary = T.oracle_link(sid, d)
            if not primary or t < lag:
                continue
            r = T.oracle_latest(valid, cik, T.mark(T.SESSIONS[t - lag]), 3)
            if r is not None and (d - valid[r][1].date()).days <= 550:
                out[t, i] = T.FF[valid[r][2]][("sic2", "ff12", "ff49").index(kind)]
    return out


def same_nan(a, b):
    return (a == b) | (np.isnan(a) & np.isnan(b))


class SicEventsOverride(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.fx = T.Fixture(Path(cls.temp.name))
        b = cls.fx.base
        cls.bridge, cls.events = b / "bridge", b / "events"
        cls.bridge_sha, cls.events_sha = T.write_bridge(cls.bridge), T.write_events(cls.events)
        cls.stage, cls.stage_diff = b / "stage", b / "stage-diff"
        cls.stage_sha, cls.stage_diff_sha = write_stage(cls.stage, SAME), write_stage(cls.stage_diff, DIFF)
        cls.plain = cls.produce("plain", T.ISSUER_RUN)
        cls.same = cls.produce("same", T.ISSUER_RUN, sic_events=cls.stage, sic_events_sha256=cls.stage_sha)
        cls.diff = cls.produce("diff", T.ISSUER_RUN, sic_events=cls.stage_diff, sic_events_sha256=cls.stage_diff_sha)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @classmethod
    def produce(cls, out, fields, **kw):
        kw.setdefault("identity_bridge", cls.bridge)
        kw.setdefault("identity_bridge_sha256", cls.bridge_sha)
        kw.setdefault("fund_events", cls.events)
        kw.setdefault("fund_events_sha256", cls.events_sha)
        return cls.fx.run(out, fields=list(fields), **kw)

    def entries(self, manifest):
        return {e["name"]: e for e in manifest["fields"]}

    def test_without_the_option_every_payload_is_the_base_commit_bytes(self):
        self.assertEqual(files_digest(self.plain), GOLDEN_ISSUER_FILES)
        self.assertNotIn("sic_events_override", self.plain["source_checks"]["issuer"])
        self.assertFalse(any("sic_events_override" in e for e in self.plain["fields"]))

    def test_same_rows_every_payload_byte_identical(self):
        self.assertEqual(self.same["files"], self.plain["files"])
        a, b = self.entries(self.plain), self.entries(self.same)
        for name in T.ISSUER_RUN:
            if name not in GRP:
                self.assertEqual(b[name], a[name], name)
                continue
            x, y = dict(a[name]), dict(b[name])
            self.assertEqual(y.pop("sic_events_override"), tool.SIC_STAGE_MAPPING)
            self.assertEqual([Path(s["path"]).name for s in y["sources"]],
                             ["manifest.json", "links.parquet", "manifest.json", "sic_events.parquet"])
            self.assertEqual(y["sources"][2]["sha256"], self.stage_sha)
            self.assertEqual(y["sources"][:2], x["sources"][:2])
            x.pop("sources"), y.pop("sources")
            self.assertEqual(y, x, name)   # clock, definition, coverage, nan reasons: unchanged
        st, st0 = self.same["source_checks"]["issuer"], self.plain["source_checks"]["issuer"]
        self.assertEqual(st["sic_events_override"]["manifest_sha256"], self.stage_sha)
        for k in ("rows_total", "rows_available_on_or_after_2025_dropped", "rows_ignored_unlinked_cik", "rows_used",
                  "rows_used_invalid_sic_skipped", "rows_sharing_cik_and_clock"):
            self.assertEqual(st["sic_events"][k], st0["sic_events"][k], k)
        self.assertEqual((st["sic_events"]["rows_by_sic_basis"], st["sic_events"]["rows_used_carried"],
                          st["sic_events"]["rows_stage_labels_differ_from_builder_mapping"]),
                         ({"fsds_sub": len(SAME), "carried": 0}, 0, 0))
        self.assertEqual(st["sic_mapping"], st0["sic_mapping"])
        self.assertEqual(st["fund_events"], st0["fund_events"])

    def test_different_rows_move_grp_cells_only_where_the_tables_disagree(self):
        for name in T.ISSUER_RUN:
            if name not in GRP:
                self.assertEqual(self.diff["files"][f"{name}.f64"], self.plain["files"][f"{name}.f64"], name)
        moved = np.zeros((len(T.SESSIONS), len(T.IDS)), dtype=bool)
        for kind in ("sic2", "ff12", "ff49"):
            got = self.fx.field("diff", f"grp_{kind}")
            np.testing.assert_array_equal(got, oracle_grp(DIFF, kind), kind)
            np.testing.assert_array_equal(self.fx.field("plain", f"grp_{kind}"), oracle_grp(SAME, kind), kind)
            moved |= ~same_nan(got, self.fx.field("plain", f"grp_{kind}"))
        a, b = T.IDS.index(101), T.IDS.index(202)
        want = np.zeros_like(moved)
        want[T.t_of("2024-10-25"):, a] = True                  # 1001: 3674 -> 2834 from its s0 + 1
        stale = [t for t, d in enumerate(T.SESSIONS) if (d - dt.date(2023, 5, 1)).days > 550 and d <= dt.date(2024, 12, 2)]
        want[stale, b] = True                                  # 2002: the carried row keeps 6022 fresh
        np.testing.assert_array_equal(moved, want)
        self.assertTrue(stale)
        self.assertEqual(self.fx.field("diff", "grp_ff12")[T.t_of("2024-11-04"), b], 11)
        self.assertEqual(self.diff["source_checks"]["issuer"]["sic_events"]["rows_used_carried"], 1)

    def test_grp_only_needs_no_fund_events(self):
        m = self.produce("grp-only", ["grp_ff12"], fund_events=None, fund_events_sha256=None,
                         sic_events=self.stage, sic_events_sha256=self.stage_sha)
        self.assertEqual(m["files"]["grp_ff12.f64"], self.same["files"]["grp_ff12.f64"])
        self.assertNotIn("fund_events_manifest", m["source_checks"]["issuer"])

    def test_refusals_fail_closed(self):
        cases = [("no-pin", T.ISSUER_RUN, {"sic_events": self.stage}, "go together"),
                 ("no-dir", T.ISSUER_RUN, {"sic_events_sha256": self.stage_sha}, "go together"),
                 ("no-grp", ["be"], {"sic_events": self.stage, "sic_events_sha256": self.stage_sha}, "no grp_"),
                 ("bad-pin", ["grp_ff12"], {"sic_events": self.stage, "sic_events_sha256": "0" * 64},
                  "sic-events manifest SHA-256"),
                 ("v1-table", ["grp_ff12"], {"sic_events": self.events, "sic_events_sha256": self.events_sha},
                  "not a complete atx.alpha-panel.fundamentals/v2"),
                 ("fund-needs-events", ["be", "grp_ff12"], {"sic_events": self.stage, "sic_events_sha256": self.stage_sha,
                                                            "fund_events": None}, "fund-events")]
        for out, fields, kw, pattern in cases:
            with self.assertRaisesRegex(ValueError, pattern):
                self.produce(out, fields, **kw)
            self.assertFalse((self.fx.base / out / "manifest.json").exists(), out)

    def test_reuse_never_crosses_sic_tables(self):
        plain_dir, same_dir = self.fx.base / "plain", self.fx.base / "same"
        r1 = self.produce("r1", T.ISSUER_RUN, sic_events=self.stage, sic_events_sha256=self.stage_sha,
                          reuse=plain_dir, reuse_sha256=T.sha(plain_dir / "manifest.json"))
        self.assertEqual(r1["reuse"]["computed"], list(GRP))
        for name in GRP:
            self.assertIn("outside this run's inputs", r1["reuse"]["not_reused"][name])
        r2 = self.produce("r2", T.ISSUER_RUN, reuse=same_dir, reuse_sha256=T.sha(same_dir / "manifest.json"))
        self.assertEqual(r2["reuse"]["computed"], list(GRP))
        r3 = self.produce("r3", T.ISSUER_RUN, sic_events=self.stage, sic_events_sha256=self.stage_sha,
                          reuse=same_dir, reuse_sha256=T.sha(same_dir / "manifest.json"))
        self.assertEqual((r3["reuse"]["computed"], r3["reuse"]["reused"]), ([], T.ISSUER_RUN))
        for m in (r1, r2, r3):
            self.assertEqual(m["files"], self.plain["files"])
        r4 = self.produce("r4", T.ISSUER_RUN, sic_events=self.stage_diff, sic_events_sha256=self.stage_diff_sha,
                          reuse=same_dir, reuse_sha256=T.sha(same_dir / "manifest.json"))
        self.assertEqual(r4["reuse"]["computed"], list(GRP))   # same producer, another stage directory
        self.assertEqual(r4["files"], self.diff["files"])

    def test_cli_accepts_the_stage_directory_or_its_parquet(self):
        for out, path in (("cli-dir", self.stage), ("cli-file", self.stage / "sic_events.parquet")):
            with contextlib.redirect_stdout(io.StringIO()):
                tool.main(["--role", str(self.fx.role), "--role-sha256", self.fx.role_sha, "--output",
                           str(self.fx.base / out), "--fields", "grp_ff12,be", "--finra", str(self.fx.finra),
                           "--tickerhistory", str(self.fx.th), "--lake", str(self.fx.lake),
                           "--identity-bridge", str(self.bridge), "--identity-bridge-sha256", self.bridge_sha,
                           "--fund-events", str(self.events), "--fund-events-sha256", self.events_sha,
                           "--sic-events", str(path), "--sic-events-sha256", self.stage_sha])
            m = json.loads((self.fx.base / out / "manifest.json").read_bytes())
            for name in ("be", "grp_ff12"):
                self.assertEqual(m["files"][f"{name}.f64"], self.same["files"][f"{name}.f64"], (out, name))
            self.assertTrue(math.isfinite(np.nanmax(self.fx.field(out, "grp_ff12"))))


if __name__ == "__main__":
    unittest.main()
