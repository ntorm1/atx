"""research_fields_v9 ``earn_season_rank`` (library v9 draft C-4 earn_season, spec F-L2; lane FIELDS-V9).

The synthetic world of test_research_fields_v8_quarters.py (dated 2014-2021: a role of the NYSE sessions 2021-03-01 ..
2021-06-30, 13 issuers with quarterly atx.fundamental-events/v1 rows from 2014: an amendment, a missing quarter, an
annual-only filer, a short history, a null period_end, a split) plus two planted cases: an amendment of an older
quarter filed after the newer quarter (its anchor is not the latest: NaN while it is the selected row) and an issuer
whose quarterly net income is rounded to whole millions (tied values: average ranks). Every cell is re-derived by an
oracle written from the definition (the QUARTER_RULE view of research_fields_v8's test oracle; ranks by sorting).
The draft module is registered only inside this file's tests (prepare_research_fields_draft.register, undone on exit).
"""
import contextlib
import datetime as dt
import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np

import prepare_research_fields as tool
import research_fields_v8 as v8
import research_fields_v9 as v9
import research_window as rw
import test_research_fields_v8_quarters as v8q
from test_research_fields_v9_nt import drafted
from test_research_window import isolated

NAME = "earn_season_rank"
SESSIONS, SIDS, N_LINES, CUT, mark = v8q.SESSIONS, v8q.SIDS, v8q.N_LINES, v8q.CUT, v8q.mark
TOOLS = Path(__file__).resolve().parent
BOUND_SEAL = dt.datetime.combine(rw.SEAL, dt.time(0), tzinfo=dt.timezone.utc)
OLD_AMEND = dt.datetime(2021, 3, 15, 15, tzinfo=dt.timezone.utc)   # 5001 amends its 2020-09-30 quarter
TIED = 5010


def world_rows():
    rows = []
    for r in v8q.events_rows():
        if r["cik"] == TIED:
            r = {**r, "ni_q": float(round(r["ni_q"] / 1e6)) * 1e6}       # whole millions: tied quarters
        rows.append(r)
    old = next(r for r in rows if r["cik"] == 5001 and r["period_end"] == dt.date(2020, 9, 30))
    rows.append({**old, "accepted_utc": OLD_AMEND, "accession": "0000005001-21-000555", "ni_q": old["ni_q"] * 1.3})
    return rows


def average_ranks(x):
    """Ascending average ranks 1..n by sorting: tied values share the mean of their positions."""
    order = sorted(x)
    return [(order.index(v) + 1 + len(order) - order[::-1].index(v)) / 2 for v in x]


def oracle(rows, seal=BOUND_SEAL):
    out = np.full((len(SESSIONS), N_LINES), np.nan)
    for t in range(len(SESSIONS)):
        for i, sid in enumerate(SIDS):
            view = v8q.selected_view(rows, sid, t, seal)
            if view is None:
                continue
            anchors = [r["period_end"] for r in view if r["period_end"] is not None]
            if view[-1]["period_end"] is None or view[-1]["period_end"] < max(anchors):
                continue                                                # an amendment of an older quarter
            qv = v8q.quarter_view(view, 23)
            x = [v8q.val(qv[k], "ni_q") for k in range(3, 23)]          # U-4 .. U-23 (U = A + 1)
            if not all(math.isfinite(v) for v in x):
                continue
            ranks = average_ranks(x)
            out[t, i] = sum(ranks[p] for p in (0, 4, 8, 12, 16)) / 5    # U-4, U-8, U-12, U-16, U-20
    return out


def entry(manifest, name=NAME):
    return next(e for e in manifest["fields"] if e["name"] == name)


class EarnSeason(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stack = contextlib.ExitStack()
        cls.stack.enter_context(drafted())
        cls.temp = cls.stack.enter_context(tempfile.TemporaryDirectory())
        cls.w = v8q.World(Path(cls.temp), rows=world_rows())
        cls.manifest = cls.w.run("es", fields=[NAME])
        cls.got = cls.w.field("es", NAME)

    @classmethod
    def tearDownClass(cls):
        cls.stack.close()

    def test_values_equal_the_definition(self):
        np.testing.assert_array_equal(self.got, oracle(self.w.rows))
        fin = self.got[np.isfinite(self.got)]
        self.assertGreater(len(fin), 400)
        self.assertTrue(((fin >= 3) & (fin <= 18)).all())
        # trending net income ranks the latest same-quarter year highest (mostly 12.0); the split, the sign flips
        # and the tied issuer give other values
        self.assertGreaterEqual(len(set(fin.tolist())), 4)
        for j in (3, 5, 12, 13, 14):          # a missing quarter in the 20, annual-only, short history, J, unlinked
            self.assertTrue(np.isnan(self.got[:, j]).all(), j)
        # the rounded issuer: its 20 ranked quarters hold tied values at the last session, and it has a value there
        view = v8q.selected_view(self.w.rows, SIDS[TIED - 5000], len(SESSIONS) - 1, BOUND_SEAL)
        ranked = [v8q.val(r, "ni_q") for r in v8q.quarter_view(view, 23)[3:23]]
        self.assertLess(len(set(ranked)), 10)
        self.assertTrue(np.isfinite(self.got[-1, TIED - 5000]))

    def test_older_quarter_amendment_is_nan_while_selected(self):
        j = 1                                  # CIK 5001
        day = lambda d: SESSIONS.index(d)
        before, during, stale = day(dt.date(2021, 3, 15)), day(dt.date(2021, 3, 16)), day(dt.date(2021, 4, 16))
        self.assertTrue(np.isfinite(self.got[before, j]))           # the 2020-12-31 row is selected
        self.assertTrue(np.isnan(self.got[during:stale + 1, j]).all())   # the 2020-09-30 amendment is selected
        e = entry(self.manifest)
        self.assertGreater(e["nan_reasons_member_cells"]["anchor_not_latest_quarter"], 0)
        self.assertGreater(e["nan_reasons_member_cells"]["fewer_than_20_finite_quarters"], 0)
        q1 = next(t for t in range(len(SESSIONS)) if np.isfinite(self.got[t, j]) and t > stale)
        self.assertGreater(q1, stale)                                # the 2021-03-31 row brings a value back

    def test_hand_computed_ranks(self):
        """One CIK, 23 quarters (k = 0 newest). x_k for k = 3..22 in order: the same-quarter positions k = 3, 7, 11,
        15, 19 hold 4, 2, 2, 9, 0; the other 15 hold 1, 2, 3 five times each. Sorted: 0 (rank 1), five 1s (2-6, mean
        4), seven 2s (7-13, mean 10), five 3s (14-18, mean 16), 4 (19), 9 (20); EarnRank = (19 + 10 + 10 + 20 + 1) / 5
        = 12. All equal: 10.5. Same-quarter values largest: (16 + 17 + 18 + 19 + 20) / 5 = 18."""
        same = {3: 4.0, 7: 2.0, 11: 2.0, 15: 9.0, 19: 0.0}
        others = iter([1.0, 2.0, 3.0] * 5)
        by_k = [100.0, 100.0, 100.0] + [same[k] if k in same else next(others) for k in range(3, 23)]
        self.assertEqual(self._rank_of(by_k), (12.0, 0))
        self.assertEqual(self._rank_of([7.0] * 23), (10.5, 0))
        big = [0.0] * 3 + [100.0 + k if k in same else float(k) for k in range(3, 23)]
        self.assertEqual(self._rank_of(big), (18.0, 0))
        low = [0.0] * 3 + [-100.0 - k if k in same else float(k) for k in range(3, 23)]
        self.assertEqual(self._rank_of(low), (3.0, 0))
        self.assertEqual(self._rank_of(by_k[:-1] + [math.nan])[1], 2)        # a missing quarter: NaN
        value, code = self._rank_of(by_k, amend_k=5)                          # newest row amends quarter k = 5
        self.assertTrue(math.isnan(value))
        self.assertEqual(code, 1)

    @staticmethod
    def _rank_of(by_k, amend_k=None):
        n = len(by_k)
        ends = v8q.quarter_ends(dt.date(2015, 3, 31), dt.date(2021, 12, 31))[:n]
        days = [v8q.day(p) for p in ends]
        ni = list(reversed(by_k))
        if amend_k is not None:
            days.append(days[n - 1 - amend_k])
            ni.append(ni[n - 1 - amend_k])
        m = len(days)
        ev = {"clock": np.arange(m, dtype=np.int64), "cidx": np.zeros(m, dtype=np.int64),
              "period_end": np.array(days, dtype=np.int64), "known": np.ones(m, dtype=bool),
              "values": {"ni_q": np.array(ni, dtype=float)}}
        value, reason = v9.earn_rank_history(ev, v8.QuarterIndex(ev))
        return float(value[-1]), int(reason[-1])

    def test_average_ranks_match_brute_force(self):
        rng = np.random.default_rng(5)
        x = rng.integers(0, 6, (200, 20)).astype(float)
        got = v9.average_ranks(x)
        for r in range(len(x)):
            self.assertEqual(got[r].tolist(), average_ranks(x[r].tolist()))

    def test_manifest_entry(self):
        e = entry(self.manifest)
        self.assertEqual((e["formula_id"], e["clock"], e["fund_lag_sessions"], e["domain"]),
                         ("chss-earnrank-ni20q-v1", v8.ISSUER_CLOCK, v8q.LAG, [3.0, 18.0]))
        self.assertEqual(e["formula_sha256"], tool.formula_id(NAME, tool.spec_definition(NAME, 0)))
        self.assertEqual(e["producer"]["module"], "research_fields_v9.py")
        self.assertEqual((e["identity_bridge_manifest_sha256"], e["fund_events_manifest_sha256"]),
                         (self.w.bridge_sha, self.w.events_sha))
        self.assertEqual(e["imported_code"], v9.imported_code("v9_earnseason"))
        self.assertEqual(v9.entry_inputs(e), v9.reuse_inputs(NAME, self.w.options()))
        checks = self.manifest["source_checks"]["v9"][NAME]
        self.assertEqual((checks["fund_lag_sessions"], checks["quarter_rule"]), (v8q.LAG, v8.QUARTER_RULE))
        self.assertEqual(checks["fund_events"]["rows_used"], len(self.w.rows))

    def test_field_at_t_unchanged_when_rows_after_t_mutate(self):
        cut_mark = mark(SESSIONS[CUT])
        rows = [{**r, "ni_q": r["ni_q"] * 1.7} if r["accepted_utc"] >= cut_mark else r for r in self.w.rows]
        q4 = next(r for r in rows if r["cik"] == 5008 and r["period_end"] == dt.date(2020, 12, 31))
        rows += [{**q4, "accepted_utc": cut_mark, "accession": "0000005008-21-000777",
                  "period_end": dt.date(2021, 3, 31), "ni_q": -5e6},              # the next quarter, early
                 {**q4, "cik": 5009, "accepted_utc": cut_mark + dt.timedelta(hours=1),
                  "accession": "0000005009-21-000777", "period_end": dt.date(2020, 9, 30)}]   # an older quarter
        late = v8q.World(self.w.base, rows=rows, name="late")
        late.run("late-es", fields=[NAME])
        row = N_LINES * 8
        before = (self.w.base / "es" / f"{NAME}.f64").read_bytes()
        after = (self.w.base / "late-es" / f"{NAME}.f64").read_bytes()
        self.assertEqual(after[:(CUT + 1) * row], before[:(CUT + 1) * row])        # rows 0..t bit-identical
        self.assertNotEqual(after[(CUT + 1) * row:], before[(CUT + 1) * row:])
        np.testing.assert_array_equal(late.field("late-es", NAME), oracle(rows))

    def test_seal_from_the_research_window(self):
        seal = mark(SESSIONS[CUT])
        extra = [{**r, "accepted_utc": seal + dt.timedelta(days=1, hours=j), "accession": f"{r['cik']:010d}-21-555555",
                  "period_end": dt.date(2021, 3, 31), "ni_q": 50 * r["ni_q"]}
                 for j, r in enumerate(x for x in self.w.rows if x["period_end"] == dt.date(2020, 12, 31))]
        loud = v8q.World(self.w.base, rows=self.w.rows + extra, name="loud")
        with mock.patch.object(tool, "SEAL_NS", int(seal.timestamp()) * 10 ** 9):
            m = self.w.run("sealed", fields=[NAME])
            m_loud = loud.run("sealed-loud", fields=[NAME])
        dropped = sum(1 for r in self.w.rows if r["accepted_utc"] >= seal)
        key = "rows_available_on_or_after_2025_dropped"     # the builder's historical name for the sealed count
        self.assertEqual(m["source_checks"]["v9"][NAME]["fund_events"][key], dropped)
        self.assertEqual(m_loud["source_checks"]["v9"][NAME]["fund_events"][key], dropped + len(extra))
        self.assertEqual(m_loud["files"][f"{NAME}.f64"], m["files"][f"{NAME}.f64"])
        np.testing.assert_array_equal(self.w.field("sealed", NAME), oracle(self.w.rows, seal=seal))
        unsealed = loud.run("unsealed-loud", fields=[NAME])
        self.assertNotEqual(unsealed["files"][f"{NAME}.f64"], self.manifest["files"][f"{NAME}.f64"])

    def test_repository_window_drops_every_row_dated_2024_or_later(self):
        """Fresh interpreter, repository window (seal 2024-01-01): events rows accepted in 2024 are dropped as sealed;
        the payload equals the one built here (where those rows come after the role and are never selected)."""
        late = [{**r, "accepted_utc": dt.datetime(2024, 2, 1, 12, tzinfo=dt.timezone.utc),
                 "accession": f"{r['cik']:010d}-24-000999", "period_end": dt.date(2023, 12, 31)}
                for r in self.w.rows if r["period_end"] == dt.date(2021, 3, 31)]
        w = v8q.World(self.w.base, rows=self.w.rows + late, name="w2024")
        here = w.run("here-2024", fields=[NAME])
        self.assertEqual(here["files"][f"{NAME}.f64"], self.manifest["files"][f"{NAME}.f64"])
        code = ("import json, sys, contextlib, io\nfrom pathlib import Path\n"
                "import prepare_research_fields as t, prepare_research_fields_draft as d, research_window as rw\n"
                "d.register(vars(t))\na = json.loads(sys.argv[1])\no = a['options']\n"
                "o.update(identity_bridge=Path(o['identity_bridge']), fund_events=Path(o['fund_events']))\n"
                "with contextlib.redirect_stdout(io.StringIO()):\n"
                "    m = t.run(Path(a['role']), a['role_sha'], Path(a['out']), ['earn_season_rank'],\n"
                "              module_options=o)\n"
                "c = m['source_checks']['v9']['earn_season_rank']['fund_events']\n"
                "print(json.dumps({'seal': rw.SEAL_DATE, 'payload': m['files']['earn_season_rank.f64'],\n"
                "                  'events': c}))\n")
        opts = {k: (str(v) if isinstance(v, Path) else v) for k, v in w.options().items()}
        got = isolated(TOOLS, code, json.dumps({"role": str(w.role), "role_sha": w.role_sha,
                                                "out": str(w.base / "repo-2024"), "options": opts}))
        self.assertEqual(got["seal"], "2024-01-01")
        self.assertEqual(got["events"]["rows_available_on_or_after_2025_dropped"], len(late))
        self.assertEqual(got["payload"], self.manifest["files"][f"{NAME}.f64"])

    def test_byte_identity_with_the_v8_quarter_fields(self):
        alone = self.w.run("eps", fields=["eps_consist_4y"])
        both = self.w.run("eps-es", fields=["eps_consist_4y", NAME])
        self.assertEqual(both["files"]["eps_consist_4y.f64"], alone["files"]["eps_consist_4y.f64"])
        self.assertEqual(entry(both, "eps_consist_4y"), entry(alone, "eps_consist_4y"))
        self.assertEqual(both["source_checks"]["v8"], alone["source_checks"]["v8"])
        self.assertEqual(set(both["source_checks"]) - set(alone["source_checks"]), {"v9"})
        self.assertEqual(both["files"][f"{NAME}.f64"], self.manifest["files"][f"{NAME}.f64"])

    def test_reuse(self):
        again = self.w.run("again", fields=[NAME], reuse=self.w.base / "es")
        self.assertEqual((again["reuse"]["reused"], again["reuse"]["computed"]), ([NAME], []))
        e = dict(entry(again))
        self.assertEqual(e.pop("reused_from")["inputs"], v9.entry_inputs(entry(self.manifest)))
        self.assertEqual(e, entry(self.manifest))
        self.w.run("eps-prior", fields=["eps_consist_4y"])
        mixed = self.w.run("mixed", fields=["eps_consist_4y", NAME], reuse=self.w.base / "eps-prior")
        self.assertEqual((mixed["reuse"]["reused"], mixed["reuse"]["computed"]), (["eps_consist_4y"], [NAME]))
        lag2 = self.w.run("lag2", fields=[NAME], fund_lag_sessions=2, reuse=self.w.base / "es")
        self.assertIn("inputs differ", lag2["reuse"]["not_reused"][NAME])
        moved = {**v9.imported_code("v9_earnseason"), "sha256": "0" * 64}
        with mock.patch.object(v9, "imported_code", lambda group, *a, **k: moved):
            edited = self.w.run("edited", fields=[NAME], reuse=self.w.base / "es")
        self.assertIn("inputs differ", edited["reuse"]["not_reused"][NAME])

    def test_refusals_before_output(self):
        for bad in ("fund_events", "identity_bridge_sha256", "fund_lag_sessions"):
            opts = {k: v for k, v in self.w.options().items() if k != bad}
            with self.assertRaisesRegex(ValueError, "--" + bad.replace("_", "-")):
                self.w.run("refused", fields=[NAME], module_options=opts)
            self.assertFalse((self.w.base / "refused").exists())
        with self.assertRaisesRegex(ValueError, "must be an integer"):
            self.w.run("refused", fields=[NAME], module_options=self.w.options(fund_lag_sessions=9))
        self.assertFalse((self.w.base / "refused").exists())


if __name__ == "__main__":
    unittest.main()
