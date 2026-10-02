"""research_fields_divevent (platform v8 lane YDATA): synthetic vendor source and role only (lane XDATA's world).

The world of test_research_fields_xdata.py (2012-06 .. 2019-05; role 2018-07-02 .. 2019-04-30) with one change: line
22, a quarterly payer, pays its last dividend in June 2018 (an omission, detected 126 sessions later). Line 66 initiates
in July 2018; 33 pays monthly, 44 annually (never an omission: not a quarterly payer), 55 never, 77 quarterly with a
special above the band. Every cell is checked against the definition written out here on the hand-listed calendar and
the independent ex-date ledger of the XDATA test; a look-ahead probe (every line pays 1% from one session on) is shown
to fail on a same-session variant."""
import contextlib
import datetime as dt
import hashlib
import math
from pathlib import Path
import tempfile
import unittest
import unittest.mock as mock

import numpy as np

import prepare_research_fields as tool
import prepare_research_fields_ydata as yreg
import research_fields_divevent as dev
import research_window as rw
import test_research_fields_xdata as txd

NAME = "div_init_omit"
STOP = (22, dt.date(2018, 7, 1))            # line 22 pays nothing on or after this date
CUT = 150                                    # the probe's mutation session (role row)
_ex_dates = txd.ex_dates


def ex_dates(cal, sid):
    out = _ex_dates(cal, sid)
    return {d for d in out if not (sid == STOP[0] and d >= STOP[1])}


@contextlib.contextmanager
def registered():
    with mock.patch.dict(tool.ALL_FIELDS), mock.patch.object(tool, "FIELD_MODULES", list(tool.FIELD_MODULES)):
        yreg.register(vars(tool))
        yield


def world():
    with mock.patch.object(txd, "ex_dates", ex_dates):
        return txd.world()


def paid_from(rows, first_day):
    """Every line pays 1% from ``first_day`` on: factor up, raw close down, on every row dated on or after it."""
    late = {k: list(v) for k, v in rows.items()}
    for i, d in enumerate(late["tradingDate"]):
        if txd.day(d) >= first_day:
            late["cumulReturnFactor"][i] /= 0.99
            late["close"][i] *= 0.99
    return late


def oracle(rows, days):
    """The definition on the calendar list, from the XDATA test's independent ledger."""
    obs = txd.observations(rows, seal=rw.SEAL)
    cal = txd.calendar()
    pos = {d: i for i, d in enumerate(cal)}
    out = np.full((len(days), len(txd.ROLE_IDS)), np.nan)
    for j, sid in enumerate(txd.ROLE_IDS):
        ex = sorted(pos[d] for d in txd.ref_events(obs, sid) if d in pos)
        seen = {pos[d] for (s, d) in obs if s == sid and d in pos}
        first = min(seen)
        for t, d in enumerate(days):
            e = pos[d]
            vis = e - 1
            if first > vis - 504:
                continue
            init = any(e - 252 <= s <= vis and not any(s - 504 <= x <= s - 1 for x in ex) and first <= s - 504
                       for s in ex)
            past = [x for x in ex if x <= vis]
            omit = False
            if past:
                L = past[-1]
                if sum(1 for x in ex if L - 251 <= x <= L) >= 3:
                    D = L + 126
                    omit = e - 252 <= D <= vis and D in seen
            out[t, j] = float(init) - float(omit)
    return out


class DivInitOmit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stack = contextlib.ExitStack()
        cls.stack.enter_context(registered())
        cls.base = Path(cls.stack.enter_context(tempfile.TemporaryDirectory()))
        cls.rows = world()
        cls.case = txd.Case(cls.base, cls.rows)
        cls.manifest = cls.case.run("f", [NAME])
        cls.got = cls.case.field("f", NAME)

    @classmethod
    def tearDownClass(cls):
        cls.stack.close()

    def test_values_match_the_definition(self):
        want = oracle(self.rows, self.case.days)
        np.testing.assert_array_equal(self.got, want)
        col = {sid: j for j, sid in enumerate(txd.ROLE_IDS)}
        self.assertTrue((self.got[:, col[66]] == 1).sum() > 150)        # the initiation, from 2018-07-17
        self.assertTrue((self.got[:, col[22]] == -1).sum() > 60)        # the omission, from about 2018-12
        for sid in (33, 44, 77):                                        # monthly, annual, quarterly with a special
            self.assertTrue((self.got[:, col[sid]] == 0).all(), sid)
        self.assertEqual(np.nansum(np.abs(self.got[:, col[55]])), 0.0)  # the non-payer
        t = self.case.days.index(txd.day(dt.date(2018, 7, 17)))
        self.assertEqual(self.got[t - 1, col[66]], 0.0)                  # the ex-date session itself: lag 1
        self.assertEqual(self.got[t, col[66]], 1.0)
        e = next(x for x in self.manifest["fields"] if x["name"] == NAME)
        self.assertEqual(e["producer"]["module"], "research_fields_divevent.py")
        self.assertEqual(e["imported_code"], dev.imported_code("y_divevent"))
        self.assertEqual(e["formula_sha256"], tool.formula_id(NAME, tool.spec_definition(NAME, 1)))
        self.assertGreater(e["initiation_member_cells"], 0)
        self.assertGreater(e["omission_member_cells"], 0)

    def test_point_in_time_probe(self):
        """Every line pays 1% from role session CUT on: rows <= CUT identical, later rows move (the non-payer 55 and
        others initiate); with LAG_SESSIONS 0 row CUT moves (the probe has teeth)."""
        moved = paid_from(self.rows, self.case.days[CUT])
        b = txd.Case(self.base, moved, "probe")
        b.run("p", [NAME])
        y = b.field("p", NAME)
        self.assertEqual(self.got[:CUT + 1].tobytes(), y[:CUT + 1].tobytes())
        self.assertNotEqual(self.got[CUT + 1:].tobytes(), y[CUT + 1:].tobytes())
        with mock.patch.object(dev, "LAG_SESSIONS", 0):
            self.case.run("la", [NAME])
            b.run("lb", [NAME])
        self.assertNotEqual(self.case.field("la", NAME)[:CUT + 1].tobytes(), b.field("lb", NAME)[:CUT + 1].tobytes())

    def test_refusals_seal_reuse(self):
        with self.assertRaisesRegex(ValueError, "need --price-source"):
            self.case.run("x1", [NAME], module_options={})
        self.assertFalse((self.base / "x1").exists())
        with mock.patch.object(tool, "SEAL", dt.date(2026, 1, 1)):
            with self.assertRaisesRegex(rw.SealError, "not research_window's"):
                self.case.run("x2", [NAME])
        self.assertFalse((self.base / "x2" / "manifest.json").exists())
        sha = hashlib.sha256((self.base / "f" / "manifest.json").read_bytes()).hexdigest()
        m = self.case.run("r", [NAME], reuse=self.base / "f", reuse_sha256=sha)
        self.assertEqual(m["reuse"]["reused"], [NAME])
        self.assertEqual((self.base / "r" / f"{NAME}.f64").read_bytes(), (self.base / "f" / f"{NAME}.f64").read_bytes())


class Pure(unittest.TestCase):
    def test_event_rows_by_hand(self):
        n_ext = 1200
        ev = np.zeros((n_ext, 3), dtype=bool)
        ev[[100, 163, 226, 289, 352], 0] = True          # line 0: quarterly, last ex-date 352 -> omission D = 478
        ev[700, 1] = True                                 # line 1: initiates at 700 (no ex-date in 196..699)
        ev[[600, 663, 726, 789], 2] = True                # line 2: initiates at 600, keeps paying
        first = np.array([0, 0, 0])
        m = dev.event_matrices(ev, first, np.ones((n_ext, 3), dtype=bool))
        self.assertTrue(math.isnan(dev.row_value(m, 504)[0]))       # vis 503 - 504 < first: short history
        self.assertEqual(dev.row_value(m, 506).tolist(), [-1.0, 0.0, 0.0])   # D = 478 in [254, 505]
        self.assertEqual(dev.row_value(m, 700).tolist(), [-1.0, 0.0, 1.0])   # line 1's ex-date session: lag 1
        self.assertEqual(dev.row_value(m, 701).tolist(), [-1.0, 1.0, 1.0])
        self.assertEqual(dev.row_value(m, 731).tolist(), [0.0, 1.0, 1.0])    # D = 478 < 731 - 252: expired
        self.assertEqual(dev.row_value(m, 853).tolist(), [0.0, 1.0, 0.0])    # 600 < 853 - 252; line 2 still pays
        m2 = dev.event_matrices(ev, first, np.zeros((n_ext, 3), dtype=bool))
        self.assertEqual(dev.row_value(m2, 506)[0], 0.0)                     # not observed at D: no omission


class Plain(unittest.TestCase):
    def test_plain_builder_does_not_register_the_module(self):
        self.assertNotIn(NAME, tool.ALL_FIELDS)
        self.assertFalse(any(type(m).__module__ == "research_fields_divevent" for m in tool.FIELD_MODULES))


if __name__ == "__main__":
    unittest.main()
