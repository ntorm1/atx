"""research_fields_deals (platform v8 lane YDATA): synthetic sec_filings stages only (the v9 NT test's world).

The role, SEC bridge and stage writer of test_research_fields_v9_nt.py (NYSE sessions 2022-07-01 .. 2023-06-30, 14
lines), with merger forms planted in filings.parquet: a proxy closed by a 25-NSE, a tender offer (SC14D9C, SC 14D9) open
to the role end, a deal before the role that times out after 252 sessions, an acquirer's proxy (an S-4 within a year
before it: dropped) and one whose S-4 is older (kept), a proxy amendment (ignored), a 22:30 UTC acceptance (usable two
sessions later), an information statement closed by a 15-12G, a Form 25 before a deal (does not close it), a 20-F filer
(NaN), an unlinked CIK, rows after the role and after the seal. Every cell is re-derived by a brute-force oracle from
the definition; a look-ahead probe is shown to fail on a same-session clock."""
import contextlib
import datetime as dt
import hashlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np

import prepare_research_fields as tool
import prepare_research_fields_ydata as yreg
import research_fields_deals as deals
import research_fields_sec as sec
import research_fields_v9 as v9
import research_window as rw
import test_research_fields_v9_nt as tnt

D, at, mark = tnt.D, tnt.at, tnt.mark
SESSIONS, IDS, CAL, IDX = tnt.SESSIONS, tnt.IDS, tnt.CAL, tnt.IDX
NAME = "deal_pending"
CUT = tnt.SESSIONS.index(D("2023-03-01"))

DEALS = [  # (cik, form, available_at, is_amendment)
    (1001, "PREM14A", at(2022, 9, 1, 14), False),        # usable 09-02 ...
    (1001, "DEFM14A", at(2022, 10, 20, 14), False),
    (1001, "25-NSE", at(2022, 12, 15, 15), False),       # ... closed: usable 12-16
    (1003, "SC14D9C", at(2022, 11, 1, 21, 59), False),   # 21:59 UTC -> usable 11-02; open past the role end
    (1003, "SC 14D9", at(2022, 11, 10, 14), False),
    (1004, "DEFM14A", at(2021, 12, 1, 14), False),       # before the role: open for 252 sessions, then 0
    (1006, "S-4", at(2022, 8, 1, 14), False),            # the acquirer: its proxy within a year is dropped
    (1006, "DEFM14A", at(2022, 10, 3, 14), False),
    (1009, "S-4", at(2021, 6, 1, 14), False),            # an S-4 more than a year before: the proxy is a target's
    (1009, "DEFM14A", at(2022, 8, 15, 14), False),
    (1010, "DEFM14A/A", at(2022, 9, 15, 14), True),      # an amendment: ignored
    (1011, "DEFM14A", at(2023, 1, 10, 22, 30), False),   # 22:30 UTC -> event 01-11, usable 01-12
    (1012, "PREM14C", at(2023, 2, 1, 14), False),        # usable 02-02 ...
    (1012, "15-12G", at(2023, 3, 1, 14), False),         # ... closed: usable 03-02
    (1014, "25", at(2022, 8, 1, 14), False),             # a resolution before the deal does not close it
    (1014, "DEFM14A", at(2022, 9, 1, 14), False),
    (1005, "SC 14D9", at(2022, 9, 1, 14), False),        # a 20-F filer: NaN
    (7777, "DEFM14A", at(2022, 9, 1, 14), False),        # unlinked
    (1013, "DEFM14A", at(2023, 8, 1, 14), False),        # after the role
    (1013, "DEFM14A", at(2025, 3, 3, 14), False),        # sealed (the window conftest.py binds)
]


def filings(extra=()):
    return tnt.filing_rows() + DEALS + list(extra)


@contextlib.contextmanager
def registered():
    with mock.patch.dict(tool.ALL_FIELDS), mock.patch.object(tool, "FIELD_MODULES", list(tool.FIELD_MODULES)):
        yreg.register(vars(tool))
        yield


def oracle(rows, seal=tnt.BOUND_SEAL):
    """The definition written out with sets and the hand-listed calendar."""
    orig = [(c, f, a) for c, f, a, amend in rows if not amend and a < seal]
    windows = {}
    for c, f, a in orig:
        if f not in deals.DEAL_FORMS:
            continue
        if f in deals.PROXY_FORMS and any(c2 == c and f2 in deals.ISSUER_FORMS and a - dt.timedelta(days=365) <= a2 <= a
                                          for c2, f2, a2 in orig):
            continue
        u = tnt.usable_index(a)
        later = [a2 for c2, f2, a2 in orig if c2 == c and f2 in deals.RESOLUTION_FORMS and a2 > a]
        leave = min(u + 252, tnt.usable_index(min(later)) if later else 10 ** 9)
        windows.setdefault(c, []).append((u, leave))
    out = np.full((len(SESSIONS), len(IDS)), np.nan)
    for t, d in enumerate(SESSIONS):
        cut = mark(CAL[IDX[d] - 1])
        for i, sid in enumerate(IDS):
            cik, kind = tnt.link_of(sid, d)
            if kind != "P":
                continue
            present = any(c == cik and f in v9.PERIODIC_FORMS and not amend and tnt.visible(a, d, seal)
                          and a >= cut - dt.timedelta(days=400) for c, f, a, amend in rows)
            if present:
                out[t, i] = float(any(u <= IDX[d] < v for u, v in windows.get(cik, [])))
    return out


class Pure(unittest.TestCase):
    def test_acquirer_proxies(self):
        k = lambda f: deals.ALL_FORMS.index(f)
        day = 86_400_000_000_000
        cidx = np.array([0, 0, 1, 1, 2, 2])
        avail = np.array([10, 300, 10, 500, 400, 400]) * day
        kind = np.array([k("S-4"), k("DEFM14A"), k("S-4"), k("DEFM14A"), k("S-4EF"), k("SC 14D9")])
        got = deals.acquirer_proxies(cidx, avail, kind, 365 * day)
        # cik 0: S-4 290 days before its proxy (acquirer); cik 1: 490 days before (target); cik 2: a 14D9 is never
        # an acquirer's (only merger proxies can be)
        self.assertEqual(got.tolist(), [False, True, False, False, False, False])

    def test_deal_windows(self):
        class Cal:
            @staticmethod
            def usable_from(a):
                return np.asarray(a, dtype=np.int64) // 10 + 1
        cidx = np.array([0, 0, 0, 1, 1])
        avail = np.array([100, 150, 900, 50, 10_000])
        deal = np.array([True, True, False, True, False])
        res = np.array([False, False, True, False, True])
        enter, leave, dc = deals.deal_windows(cidx, avail, deal, res, Cal())
        self.assertEqual(enter.tolist(), [11, 16, 6])
        self.assertEqual(leave.tolist(), [91, 91, 6 + deals.DEAL_MAX_SESSIONS])   # cik 1: resolution too late
        self.assertEqual(dc.tolist(), [0, 0, 1])


class DealPending(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stack = contextlib.ExitStack()
        cls.stack.enter_context(registered())
        cls.temp = cls.stack.enter_context(tempfile.TemporaryDirectory())
        cls.w = tnt.World(Path(cls.temp), filings=filings())
        cls.manifest = cls.w.run("d", fields=(NAME,))
        cls.got = cls.w.field("d", NAME)

    @classmethod
    def tearDownClass(cls):
        cls.stack.close()

    def at_(self, day, sid):
        return self.got[SESSIONS.index(D(day)), IDS.index(sid)]

    def test_values_equal_the_definition(self):
        np.testing.assert_array_equal(self.got, oracle(self.w.filings))
        self.assertGreater(int(np.nansum(self.got)), 400)
        for sid in (102, 105, 108):                                  # J line, 20-F filer, unlinked line
            self.assertTrue(np.isnan(self.got[:, IDS.index(sid)]).all(), sid)
        for sid in (106, 110, 113):                                  # acquirer proxy, amendment, after-role deal
            col = self.got[:, IDS.index(sid)]
            self.assertEqual(np.nansum(col), 0.0, sid)
            self.assertTrue((col == 0).any(), sid)

    def test_hand_computed_cells(self):
        self.assertEqual(self.at_("2022-09-01", 101), 0.0)
        self.assertEqual(self.at_("2022-09-02", 101), 1.0)
        self.assertEqual(self.at_("2022-12-15", 101), 1.0)
        self.assertEqual(self.at_("2022-12-16", 101), 0.0)         # the 25-NSE of 12-15 15:00 UTC
        self.assertEqual(self.at_("2022-11-01", 103), 0.0)
        self.assertEqual(self.at_("2022-11-02", 103), 1.0)          # 21:59 UTC: the next session
        self.assertEqual(self.at_("2023-06-30", 103), 1.0)
        last = CAL[tnt.usable_index(at(2021, 12, 1, 14)) + deals.DEAL_MAX_SESSIONS - 1]
        self.assertEqual(self.at_(last.isoformat(), 104), 1.0)      # the 252nd session of the window
        self.assertEqual(self.at_(CAL[IDX[last] + 1].isoformat(), 104), 0.0)
        self.assertEqual(self.at_("2022-08-16", 109), 1.0)          # S-4 more than a year before: a target
        self.assertEqual(self.at_("2023-01-11", 111), 0.0)          # accepted 01-10 22:30 UTC
        self.assertEqual(self.at_("2023-01-12", 111), 1.0)
        self.assertEqual(self.at_("2023-03-01", 112), 1.0)
        self.assertEqual(self.at_("2023-03-02", 112), 0.0)          # the 15-12G
        self.assertEqual(self.at_("2022-09-02", 114), 1.0)          # its earlier Form 25 does not close it
        self.assertEqual(self.at_("2023-06-30", 114), 1.0)          # still inside the 252 sessions

    def test_entry_and_checks(self):
        e = next(x for x in self.manifest["fields"] if x["name"] == NAME)
        self.assertEqual(e["producer"]["module"], "research_fields_deals.py")
        self.assertEqual(e["stage_manifests"]["sec_filings"]["sha256"], self.w.stage_sha)
        self.assertEqual(e["imported_code"], deals.imported_code("y_deal"))
        self.assertEqual(e["formula_sha256"], tool.formula_id(NAME, tool.spec_definition(NAME, 1)))
        st = self.manifest["source_checks"]["deals"]["filings"]
        self.assertEqual(st["acquirer_proxies_dropped"], 1)
        self.assertEqual(st["rows_sealed"], sum(1 for *_, a, amend in filings() if a >= tnt.BOUND_SEAL and not amend))
        self.assertGreaterEqual(st["rows_sealed"], 1)

    def test_point_in_time_probe(self):
        """Every filing available at or after the t-1 mark of CUT removed and a new deal filed on CUT: rows <= CUT
        identical, later rows move; a same-session clock (usable at the acceptance's own session) moves row CUT."""
        edge = mark(SESSIONS[CUT - 1])
        moved = [r for r in filings() if r[2] < edge] + [(1013, "DEFM14A", at(2023, 3, 1, 14), False)]
        root = Path(self.temp)
        b = tnt.World(root, filings=moved, name="probe")
        b.run("p", fields=(NAME,))
        y = b.field("p", NAME)
        self.assertEqual(self.got[:CUT + 1].tobytes(), y[:CUT + 1].tobytes())
        self.assertNotEqual(self.got[CUT + 1:].tobytes(), y[CUT + 1:].tobytes())
        same = lambda self_, a: np.searchsorted(self_.marks, a, side="right").astype(np.int64)
        with mock.patch.object(sec.Calendar, "usable_from", same):
            self.w.run("la", fields=(NAME,))
            b.run("lb", fields=(NAME,))
        self.assertNotEqual(self.w.field("la", NAME)[:CUT + 1].tobytes(), b.field("lb", NAME)[:CUT + 1].tobytes())

    def test_refusals_seal_reuse(self):
        with self.assertRaisesRegex(ValueError, "need --sec-stages"):
            self.w.run("x1", fields=(NAME,), module_options={})
        self.assertFalse((Path(self.temp) / "x1").exists())
        with mock.patch.object(tool, "SEAL", dt.date(2026, 1, 1)):
            with self.assertRaisesRegex(rw.SealError, "not research_window's"):
                self.w.run("x2", fields=(NAME,))
        self.assertFalse((Path(self.temp) / "x2" / "manifest.json").exists())
        sha = hashlib.sha256((Path(self.temp) / "d" / "manifest.json").read_bytes()).hexdigest()
        m = self.w.run("r", fields=(NAME,), reuse=Path(self.temp) / "d", reuse_sha256=sha)
        self.assertEqual(m["reuse"]["reused"], [NAME])
        self.assertEqual((Path(self.temp) / "r" / f"{NAME}.f64").read_bytes(),
                         (Path(self.temp) / "d" / f"{NAME}.f64").read_bytes())



class Plain(unittest.TestCase):
    def test_plain_builder_does_not_register_the_module(self):
        """Off by default: outside registered() the builder knows neither the field nor the module."""
        self.assertNotIn(NAME, tool.ALL_FIELDS)
        self.assertFalse(any(type(m).__module__ == "research_fields_deals" for m in tool.FIELD_MODULES))
        with registered():
            self.assertIn(NAME, tool.ALL_FIELDS)
        self.assertNotIn(NAME, tool.ALL_FIELDS)


if __name__ == "__main__":
    unittest.main()
