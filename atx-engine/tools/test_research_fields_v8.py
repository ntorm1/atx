"""research_fields_v8 (platform v8 F-3): grp_ff12f49 on a synthetic role, bridge and SIC table only.

The world: a role of the weekdays 2021-03-01 .. 2021-06-30 (five lines), a pinned identity bridge and an
atx.fundamental-events/v1 SIC table whose rows walk every FF12 Money / FF49 case (Banks, Insur, RlEst, the REIT SIC
6798 in Fin, a Money SIC without an FF49 industry), a non-Money SIC change, an unmapped FF49 (SIC 9999), a late link
and the 550-day staleness. Every field is built through the builder's run() (the FIELD_MODULES hook)."""
import contextlib
import datetime as dt
import io
import json
import math
from pathlib import Path
import tempfile
import unittest

import numpy as np

import prepare_research_fields as tool
import research_fields_holdings as hold
import research_fields_sec as sec
import research_fields_v8 as v8
import test_prepare_research_fields as base

SESSIONS = [d for d in (dt.date(2021, 3, 1) + dt.timedelta(days=i) for i in range(122)) if d.weekday() < 5]
UTC = dt.timezone.utc
AFTER_SEAL = dt.datetime.combine(tool.SEAL, dt.time(12), tzinfo=UTC) + dt.timedelta(days=5)
BRIDGE = [  # (sr_id, cik, start, end_incl, available_at, primary, tier, basis)
    (101, 1001, dt.date(2015, 1, 1), None, base.LONG_AGO, "P", "high", "reconstructed_high"),
    (202, 2002, dt.date(2015, 1, 1), None, base.LONG_AGO, "P", "high", "reconstructed_high"),
    (303, 3003, dt.date(2015, 1, 1), None, base.LONG_AGO, "P", "high", "reconstructed_high"),
    (404, 4004, dt.date(2015, 1, 1), None, base.LONG_AGO, "P", "high", "reconstructed_high"),
    (505, 5005, dt.date(2021, 4, 1), None, base.mark(dt.date(2021, 4, 1)), "P", "high", "reconstructed_high"),
]
SIC_EVENTS = [  # (cik, accepted_utc, sic, accession)
    (1001, base.at(2020, 11, 10, 15), 6022, "0000001001-20-000001"),   # Banks -> 145
    (1001, base.at(2021, 4, 14, 15), 6311, "0000001001-21-000001"),    # Insur -> 146 from 04-15 (lag 1)
    (1001, AFTER_SEAL, 6211, "0000001001-99-000001"),                  # sealed: never used
    (2002, base.at(2020, 8, 5, 15), 6512, "0000002002-20-000001"),     # RlEst -> 147
    (3003, base.at(2020, 9, 1, 15), 6798, "0000003003-20-000001"),     # REIT: FF49 48 Fin -> 148
    (3003, base.at(2021, 5, 12, 15), 6001, "0000003003-21-000001"),    # Money, no FF49 industry -> 11 from 05-13
    (4004, base.at(2020, 10, 1, 15), 7372, "0000004004-20-000001"),    # BusEq 6 (FF49 36)
    (4004, base.at(2021, 3, 24, 15), 9999, "0000004004-21-000001"),    # Other 12 (no FF49) from 03-25
    (5005, base.at(2019, 10, 15, 15), 2834, "0000005005-19-000001"),   # Hlth 10; 550 days old on 2021-04-17
]
EVENTS = [(1001, base.at(2021, 1, 20, 20), "fsds_accepted_utc", dt.date(2020, 12, 31), 200, 100.0, 1000.0,
           "0000001001-21-000009")]
GRP = ["grp_sic2", "grp_ff12", "grp_ff49"]
RUN = GRP + ["grp_ff12f49"]
CUT = 45                                   # the mutation session (role row)


def t_of(text):
    return SESSIONS.index(dt.date.fromisoformat(text))


class V8Fixture:
    def __init__(self, root: Path):
        self.base = root
        self.role = root / "role"
        self.role_sha = base.write_role(self.role, "0" * 64, sessions=SESSIONS)
        self.bridge = root / "bridge"
        self.bridge_sha = base.write_bridge(self.bridge, rows=BRIDGE)
        self.events, self.events_sha = self.write_events("events", SIC_EVENTS)

    def write_events(self, name, sic_events):
        path = self.base / name
        return path, base.write_events(path, events=EVENTS, sic_events=sic_events)

    def run(self, out, fields=RUN, **kw):
        kw.setdefault("identity_bridge", self.bridge)
        kw.setdefault("identity_bridge_sha256", self.bridge_sha)
        kw.setdefault("fund_events", self.events)
        kw.setdefault("fund_events_sha256", self.events_sha)
        if isinstance(kw.get("reuse"), str):
            kw["reuse"] = self.base / kw["reuse"]
        with contextlib.redirect_stdout(io.StringIO()):
            return tool.run(self.role, self.role_sha, self.base / out, fields=list(fields), **kw)

    def field(self, out, name):
        return np.fromfile(self.base / out / f"{name}.f64", dtype="<f8").reshape(len(SESSIONS), len(base.IDS))


def entry(manifest, name):
    return next(e for e in manifest["fields"] if e["name"] == name)


def oracle(ff12, ff49):
    """FF12F49_RULE written out cell by cell."""
    out = np.full(ff12.shape, np.nan)
    for t in range(ff12.shape[0]):
        for i in range(ff12.shape[1]):
            a, b = float(ff12[t, i]), float(ff49[t, i])
            if math.isnan(a):
                continue
            out[t, i] = 100.0 + b if a == 11.0 and b in (45.0, 46.0, 47.0, 48.0) else a
    return out


class Ff12f49(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.fx = V8Fixture(Path(cls.temp.name))
        cls.manifest = cls.fx.run("v8")
        cls.code = cls.fx.field("v8", "grp_ff12f49")
        cls.ff12, cls.ff49 = cls.fx.field("v8", "grp_ff12"), cls.fx.field("v8", "grp_ff49")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_money_mappings(self):
        c, a, b, s, d, e = self.code, 0, 1, 2, 3, 4
        self.assertTrue(np.all(np.isnan(c[0])))                           # t < lag: no SIC row visible yet
        self.assertEqual(c[t_of("2021-04-14"), a], 145)                   # Banks (6022)
        self.assertEqual(c[t_of("2021-04-15"), a], 146)                   # Insur (6311) from s0 + 1
        self.assertTrue(np.all(c[1:, b] == 147))                          # RlEst (6512)
        self.assertEqual(c[t_of("2021-05-12"), s], 148)                   # REIT 6798: FF49 48 Fin
        self.assertEqual(c[t_of("2021-05-13"), s], 11)                    # 6001: Money without an FF49 industry
        self.assertTrue(np.isnan(self.ff49[t_of("2021-05-13"), s]))
        self.assertEqual(c[t_of("2021-03-24"), d], 6)                     # 7372: non-Money keeps its FF12 code
        self.assertEqual(c[t_of("2021-03-25"), d], 12)                    # 9999: FF12 Other, FF49 unmapped
        self.assertTrue(np.all(np.isnan(c[:t_of("2021-04-01"), e])))      # not linked before 04-01
        self.assertEqual(c[t_of("2021-04-16"), e], 10)                    # 2834 Hlth, age 549
        self.assertTrue(np.all(np.isnan(c[t_of("2021-04-19"):, e])))      # age > 550: stale
        np.testing.assert_array_equal(c, oracle(self.ff12, self.ff49))
        np.testing.assert_array_equal(c, v8.ff12f49_code(self.ff12, self.ff49))
        e_ = entry(self.manifest, "grp_ff12f49")
        member = np.fromfile(self.fx.role / "member.u8", dtype="u1").reshape(c.shape) != 0
        self.assertEqual(e_["money_member_cells"], {k: int(np.count_nonzero(member & (c == float(k))))
                                                    for k in v8.MONEY_CODES})
        self.assertTrue(all(v > 0 for v in e_["money_member_cells"].values()))

    def test_non_money_cells_equal_grp_ff12_bit_for_bit(self):
        non_money = np.isfinite(self.ff12) & (self.ff12 != 11)
        self.assertGreater(int(non_money.sum()), 0)
        np.testing.assert_array_equal(self.code.view("<u8")[non_money], self.ff12.view("<u8")[non_money])
        money = self.ff12 == 11
        self.assertTrue(np.all(np.isin(self.code[money], [11.0, 145.0, 146.0, 147.0, 148.0])))

    def test_nan_pattern_equals_grp_ff12(self):
        np.testing.assert_array_equal(np.isnan(self.code), np.isnan(self.ff12))
        raw = (self.fx.base / "v8" / "grp_ff12f49.f64").read_bytes()
        nan_bytes = np.array([np.nan], dtype="<f8").tobytes()
        cells = [raw[k:k + 8] for k in range(0, len(raw), 8)]
        self.assertTrue(all(x == nan_bytes for x, n in zip(cells, np.isnan(self.code).ravel()) if n))  # canonical NaN

    def test_field_at_t_unchanged_when_rows_after_t_mutate(self):
        cut_mark = base.mark(SESSIONS[CUT])
        mutated = [(cik, when, 6211 if when >= cut_mark and sic is not None else sic, acc)
                   for cik, when, sic, acc in SIC_EVENTS]                 # every row at or after the t mark changes
        mutated += [(1001, cut_mark + dt.timedelta(hours=1), 6798, "0000001001-21-000050"),
                    (4004, base.mark(SESSIONS[CUT + 3]), 6022, "0000004004-21-000050"),
                    (2002, cut_mark, 6001, "0000002002-21-000050")]      # exactly at the t mark: after t
        path, sha = self.fx.write_events("events-mutated", mutated)
        self.fx.run("mutated", fund_events=path, fund_events_sha256=sha)
        before = (self.fx.base / "v8" / "grp_ff12f49.f64").read_bytes()
        after = (self.fx.base / "mutated" / "grp_ff12f49.f64").read_bytes()
        row = len(base.IDS) * 8
        self.assertEqual(after[:(CUT + 1) * row], before[:(CUT + 1) * row])   # rows 0..t bit-identical
        self.assertNotEqual(after[(CUT + 1) * row:], before[(CUT + 1) * row:])  # the mutation is visible later
        got = self.fx.field("mutated", "grp_ff12f49")
        self.assertEqual(got[CUT + 1, 1], 147)                            # a clock exactly at the t mark is not ...
        self.assertEqual(got[CUT + 2, 1], 11)                             # ... before it: usable from t + 2 (lag 1)
        self.assertEqual(got[CUT + 2, 0], 148)                            # 1001 -> 6798 (Fin)

    def test_existing_payloads_byte_identical(self):
        alone = self.fx.run("grp-only", fields=GRP)
        for name in GRP:
            self.assertEqual(self.manifest["files"][f"{name}.f64"], alone["files"][f"{name}.f64"], name)
            self.assertEqual(entry(self.manifest, name), entry(alone, name), name)
        self.assertEqual([e["name"] for e in self.manifest["fields"]], RUN)   # registry order: v8 fields last
        e = entry(self.manifest, "grp_ff12f49")
        self.assertEqual(e["depends_on"], ["grp_ff12", "grp_ff49"])
        self.assertEqual((e["formula_id"], e["point_in_time"], e["producer"]["module"]),
                         ("ff12-money-ff49-v1", True, "research_fields_v8.py"))
        self.assertEqual(e["producer"]["code_sha256_lf"], tool.module_code_identity(v8)["code_sha256_lf"])
        self.assertEqual({s["field"]: s["sha256"] for s in e["sources"]},
                         {x: self.manifest["files"][f"{x}.f64"]["sha256"] for x in ("grp_ff12", "grp_ff49")})

    def test_reuse_copies_unchanged_and_recomputes_after_a_group_change(self):
        again = self.fx.run("again", reuse="v8")
        block = again["reuse"]
        self.assertEqual((block["reused"], block["computed"], block["not_reused"]), (RUN, [], {}))
        self.assertEqual(again["files"], self.manifest["files"])
        e = entry(again, "grp_ff12f49")
        self.assertEqual({k: v for k, v in e.items() if k != "reused_from"}, entry(self.manifest, "grp_ff12f49"))
        self.assertEqual(e["reused_from"]["producer"]["module"], "research_fields_v8.py")
        chained = self.fx.run("again2", reuse="again")                   # a reuse of a reuse still reuses
        self.assertEqual(chained["reuse"]["reused"], RUN)
        path, sha = self.fx.write_events("events-changed", SIC_EVENTS + [
            (2002, base.at(2021, 3, 10, 15), 6311, "0000002002-21-000077")])
        changed = self.fx.run("changed", reuse="v8", fund_events=path, fund_events_sha256=sha)
        self.assertIn("grp_ff12f49", changed["reuse"]["computed"])
        self.assertIn("depends on grp_ff12, grp_ff49", changed["reuse"]["not_reused"]["grp_ff12f49"])
        self.assertEqual(self.fx.field("changed", "grp_ff12f49")[t_of("2021-03-12"), 1], 146)

    def test_requires_registration_and_cli(self):
        with self.assertRaisesRegex(ValueError, "grp_ff12f49 requires grp_ff12, grp_ff49"):
            self.fx.run("alone", fields=["grp_ff12f49"])
        self.assertEqual(list(tool.ALL_FIELDS)[-len(v8.FIELDS):], list(v8.FIELDS))   # after every other field
        self.assertIs(tool.ALL_FIELDS["grp_ff12f49"], v8.FIELDS["grp_ff12f49"])
        self.assertEqual(sum(isinstance(m, v8.V8FieldModule) for m in tool.FIELD_MODULES), 1)
        self.assertEqual({v8.producer_group(x) for x in v8.FIELDS}, set(v8.PRODUCERS))
        fps = tool.module_fingerprints(v8, tool.module_source(v8), tool.builder_source())
        self.assertTrue(all(isinstance(fps[g], str) for g in v8.PRODUCERS))
        with contextlib.redirect_stdout(io.StringIO()):
            tool.main(["--role", str(self.fx.role), "--role-sha256", self.fx.role_sha,
                       "--output", str(self.fx.base / "cli"), "--fields", ",".join(["grp_ff12f49", "grp_ff49",
                                                                                     "grp_ff12"]),
                       "--identity-bridge", str(self.fx.bridge), "--identity-bridge-sha256", self.fx.bridge_sha,
                       "--fund-events", str(self.fx.events), "--fund-events-sha256", self.fx.events_sha])
        m = json.loads((self.fx.base / "cli" / "manifest.json").read_bytes())
        self.assertEqual([f["name"] for f in m["fields"]], ["grp_ff12", "grp_ff49", "grp_ff12f49"])
        self.assertEqual(m["files"]["grp_ff12f49.f64"], self.manifest["files"]["grp_ff12f49.f64"])

    def test_registration_keeps_every_other_producer_fingerprint(self):
        with_hook = tool.builder_source()
        without = b"\n".join(x for x in with_hook.split(b"\n") if b"_v8" not in x)
        self.assertNotEqual(with_hook, without)
        self.assertEqual(tool.code_fingerprint.fingerprints(with_hook, tool.FIELD_PRODUCERS, tool.PRODUCER_ORCHESTRATION),
                         tool.code_fingerprint.fingerprints(without, tool.FIELD_PRODUCERS, tool.PRODUCER_ORCHESTRATION))
        for m in (sec, hold):   # SEC reads spec_definition (hence ALL_FIELDS) through its handle
            self.assertEqual(tool.module_fingerprints(m, tool.module_source(m), with_hook),
                             tool.module_fingerprints(m, tool.module_source(m), without), m.__name__)


if __name__ == "__main__":
    unittest.main()
