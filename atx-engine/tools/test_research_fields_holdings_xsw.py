"""exch_up_365d (platform v8 LIB2, holdings kind ``xsw``): a recent move of a line's listing up to NYSE or NYSE American.

Synthetic security_master stage only (FINRA name rows 2019-05..2021-08, role sessions 2020-03..2021-08; no row is dated
on or after 2024-01-01). Checked: the rule on hand-made row sequences; every cell against an independent per-session
replay and hand-planted cells; the look-ahead probe (rows at or after the t-1 mark mutated or added leave rows 0..t
bit-identical, and the same probe catches a one-session look-ahead injected into the visibility marks); the reader-side
seal; --reuse (self copy, a changed stage pin recomputes, producer fingerprints isolated by kind).
"""
import contextlib
import datetime as dt
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np
import pyarrow as pa

import prepare_research_fields as tool
import research_fields_holdings as hold
import research_window as rw
import test_prepare_research_fields as base
import test_research_fields_holdings as holdt

UTC = dt.timezone.utc
FIELD = "exch_up_365d"
IDS = base.IDS                                   # 101 202 303 404 505
SESSIONS = holdt.weekdays(dt.date(2020, 3, 2), dt.date(2021, 8, 31))
NAME_DAYS = [dt.date(y, m, d) for y in (2019, 2020, 2021) for m in range(1, 13) for d in (10, 25)
             if dt.date(2019, 5, 1) <= dt.date(y, m, d) <= dt.date(2021, 8, 31)]
VENUE = {"NNM": 0, "SC": 0, "AMEX": 1, "NYSE": 2}
UP = {(0, 2), (1, 2), (0, 1)}


def mark(d, hours=22):
    return dt.datetime(d.year, d.month, d.day, tzinfo=UTC) + dt.timedelta(hours=hours)


def series(sid, plan):
    """Rows (sid, dissemination date, available_at, class) on NAME_DAYS: ``plan(d)`` gives the class or None (no row)."""
    return [(sid, d, mark(d), c) for d in NAME_DAYS if (c := plan(d)) is not None]


def world_rows():
    d = dt.date
    rows = []
    # 101: Nasdaq -> NYSE, first seen on the 2020-07-10 row: flagged for 365 days after it, inside the role
    rows += series(101, lambda x: "NNM" if x < d(2020, 7, 10) else "NYSE")
    # 202: NYSE American -> NYSE across a 77-day gap (not compared: no event; stale in between), then NYSE -> Nasdaq
    # (a down move) and Nasdaq -> NYSE on 2021-05-10 (an up-switch, after the probe seal of the seal test)
    rows += series(202, lambda x: None if d(2020, 7, 25) < x < d(2020, 10, 10) else
                   ("AMEX" if x <= d(2020, 7, 25) else "NYSE" if x < d(2021, 3, 10) else
                    "NNM" if x < d(2021, 5, 10) else "NYSE"))
    # 303: Nasdaq -> NYSE American; the 2021-01-10 row is republished late (visible 2021-02-01), so the switch is first
    # seen on the 2021-01-25 row and the late row is out of order; then NYSE American -> NYSE on 2021-06-10
    for r in series(303, lambda x: "NNM" if x < d(2021, 1, 10) else "AMEX" if x < d(2021, 6, 10) else "NYSE"):
        rows.append(r if r[1] != d(2021, 1, 10) else (303, r[1], mark(d(2021, 2, 1)), r[3]))
    # 404: Nasdaq; on 2020-11-10 a second row with NYSE arrives a day later (conflict: venue other), so the NYSE row of
    # 2020-11-25 is no switch; a late row for 2020-10-25 claiming NYSE is out of order
    rows += series(404, lambda x: "NNM" if x <= d(2020, 11, 10) else "NYSE")
    rows.append((404, d(2020, 11, 10), mark(d(2020, 11, 11)), "NYSE"))
    rows.append((404, d(2020, 10, 25), mark(d(2020, 12, 1)), "NYSE"))
    # 505: Nasdaq -> NYSE Arca (other: no event), a null class, rows stop after 2021-04-10 (stale: NaN)
    rows += series(505, lambda x: None if x > d(2021, 4, 10) else "NNM" if x < d(2021, 3, 10) else
                   (None if x == d(2021, 4, 10) else "ARCA"))
    rows.append((505, d(2021, 4, 10), mark(d(2021, 4, 10)), None))
    # off the role and unmapped: never read into a line
    rows += series(999, lambda x: "NNM" if x < d(2020, 9, 10) else "NYSE")
    rows.append((0, d(2020, 9, 10), mark(d(2020, 9, 10)), "NYSE"))
    return rows


def write_names(root: Path, rows) -> str:
    table = pa.table({"security_id": pa.array([r[0] for r in rows], pa.int64()),
                      "dissemination_date": pa.array([r[1] for r in rows], pa.date32()),
                      "available_at": pa.array([r[2] for r in rows], pa.timestamp("us", tz="UTC")),
                      "market_class": pa.array([r[3] for r in rows], pa.string())})
    return holdt.write_stage(root, "security_master", {"finra_names.parquet": table})


class World:
    def __init__(self, root: Path, rows=None):
        self.base = root
        self.rows = world_rows() if rows is None else rows
        self.role = root / "role"
        self.role_sha = base.write_role(self.role, "0" * 64, SESSIONS)
        self.stage = root / "security_master"
        self.stage_sha = write_names(self.stage, self.rows)

    def run(self, out, fields=(FIELD,), **kw):
        kw.setdefault("security_master", self.stage)
        kw.setdefault("security_master_sha256", self.stage_sha)
        with contextlib.redirect_stdout(io.StringIO()):
            return tool.run(self.role, self.role_sha, self.base / out, list(fields), **kw)

    def field(self, out, name=FIELD):
        return np.fromfile(self.base / out / f"{name}.f64", dtype="<f8").reshape(len(SESSIONS), len(IDS))


def oracle(rows, seal=None):
    """Independent replay: for each session, every row visible at it (available_at < date(t-1) 22:00 UTC) replayed from
    scratch in visibility order."""
    out = np.full((len(SESSIONS), len(IDS)), np.nan)
    indexed = [(r[2], r[1], VENUE.get(r[3], -1), i, r) for i, r in enumerate(rows)
               if r[0] in IDS and (seal is None or r[2] < seal)]
    for t in range(1, len(SESSIONS)):
        cut = mark(SESSIONS[t - 1])
        vis = sorted(x for x in indexed if x[0] < cut)
        if not vis or SESSIONS[t] - dt.timedelta(days=410) < min(x[1] for x in vis):
            continue                                                       # nothing visible, or history short
        state, last = {}, {}
        for _, dd, v, _, r in vis:
            sid = r[0]
            if sid not in state:
                state[sid] = [dd, v]
            elif dd > state[sid][0]:
                if (dd - state[sid][0]).days <= 45 and (state[sid][1], v) in UP:
                    last[sid] = dd
                state[sid] = [dd, v]
            elif dd == state[sid][0] and v != state[sid][1]:
                state[sid][1] = -1
        for i, sid in enumerate(IDS):
            if sid in state and (SESSIONS[t] - state[sid][0]).days <= 45:
                out[t, i] = 1.0 if sid in last and (SESSIONS[t] - last[sid]).days <= 365 else 0.0
    return out


def t_first_after(d):
    """The first session t whose t-1 session is after date d (a row available at d 22:00 is visible from t)."""
    return next(t for t in range(1, len(SESSIONS)) if SESSIONS[t - 1] > d)


def col(sid):
    return IDS.index(sid)


class Rule(unittest.TestCase):
    def test_events_on_hand_made_sequences(self):
        # line 0: nasdaq, nasdaq, nyse (switch), amex (down), nyse (switch), then a 46-day gap to nasdaq->amex (none)
        col_ = np.array([0, 0, 0, 0, 0, 0, 0], dtype=np.int64)
        dd = np.array([0, 15, 30, 45, 60, 106, 120], dtype=np.int64)
        ven = np.array([0, 0, 2, 1, 2, 0, 1], dtype=np.int64)
        ev, sdd, sven, c = hold.xsw_events(col_, dd, ven)
        self.assertEqual(ev.tolist(), [False, False, True, False, True, False, True])
        self.assertEqual((c["up_switches"], c["advancing_beyond_gap"], c["first_rows"]), (3, 1, 1))
        self.assertEqual(sdd.tolist(), dd.tolist())
        # gap exactly 45 days is compared; 46 is not
        ev, *_ = hold.xsw_events(np.zeros(2, np.int64), np.array([0, 45]), np.array([0, 2]))
        self.assertEqual(ev.tolist(), [False, True])
        ev, *_ = hold.xsw_events(np.zeros(2, np.int64), np.array([0, 46]), np.array([0, 2]))
        self.assertEqual(ev.tolist(), [False, False])
        # a same-date conflict makes the venue other; the next row cannot switch from it; out-of-order rows are ignored
        ev, sdd, sven, c = hold.xsw_events(np.zeros(5, np.int64), np.array([0, 0, 10, 5, 20]),
                                           np.array([0, 2, 2, 0, 2]))
        self.assertEqual(ev.tolist(), [False] * 5)
        self.assertEqual(sven.tolist(), [0, -1, 2, 2, 2])
        self.assertEqual((c["conflicts"], c["out_of_order_rows"]), (1, 1))
        # other venues never switch, in either direction; lines are independent
        ev, *_ = hold.xsw_events(np.array([0, 1, 0, 1]), np.array([0, 0, 15, 15]), np.array([-1, 0, 2, 2]))
        self.assertEqual(ev.tolist(), [False, False, False, True])

    def test_declared_constants(self):
        self.assertEqual((hold.XSW_WINDOW_DAYS, hold.XSW_MAX_GAP_DAYS), (365, 45))
        self.assertEqual(hold.XSW_MAX_GAP_DAYS, hold.REGSHO_NAME_MAX_AGE_DAYS)
        self.assertEqual(hold.SEAL, rw.SEAL)
        spec = hold.HOLD_FIELDS[FIELD]
        self.assertEqual((spec["kind"], spec["formula_id"]), ("xsw", "finra-listing-up-switch365-v1"))
        self.assertEqual(hold.KIND_STAGES["xsw"], ("security_master",))
        self.assertNotIn(FIELD, tool.ALL_FIELDS)        # opt-in: the builder's own registry is unchanged


class Field(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.w = World(Path(cls.temp.name))
        cls.manifest = cls.w.run("fields")
        cls.f = cls.w.field("fields")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_every_cell_against_the_replay(self):
        np.testing.assert_array_equal(self.f, oracle(self.w.rows))

    def test_planted_cells(self):
        f, d = self.f, dt.date
        self.assertTrue(np.all(np.isnan(f[0])))                                  # session 0 sees nothing
        short = [t for t in range(len(SESSIONS)) if SESSIONS[t] - dt.timedelta(days=410) < d(2019, 5, 10)]
        self.assertTrue(np.all(np.isnan(f[short])))                              # history short of 410 days
        t101 = t_first_after(d(2020, 7, 10))
        self.assertEqual((f[t101 - 1, col(101)], f[t101, col(101)]), (0.0, 1.0))   # visible from t-1 > 2020-07-10
        last = max(t for t in range(len(SESSIONS)) if (SESSIONS[t] - d(2020, 7, 10)).days <= 365)
        self.assertEqual((f[last, col(101)], f[last + 1, col(101)]), (1.0, 0.0))   # 365 days, then 0
        quiet = slice(t_first_after(d(2020, 10, 10)), t_first_after(d(2021, 5, 10)))
        self.assertTrue(np.all(f[quiet, col(202)] == 0.0))                       # 77-day gap, then a down move
        stale = [t for t in range(len(SESSIONS)) if d(2020, 9, 9) <= SESSIONS[t] and SESSIONS[t - 1] <= d(2020, 10, 10)]
        self.assertTrue(np.all(np.isnan(f[stale, col(202)])))                   # no row for more than 45 days
        self.assertEqual(f[t_first_after(d(2021, 5, 10)), col(202)], 1.0)       # Nasdaq -> NYSE after a down move
        t303 = t_first_after(d(2021, 1, 25))
        self.assertEqual((f[t303 - 1, col(303)], f[t303, col(303)]), (0.0, 1.0))   # seen on the 2021-01-25 row
        self.assertTrue(np.all(f[t303:, col(303)] == 1.0))
        self.assertTrue(np.all(f[short[-1] + 1:, col(404)] == 0.0))              # conflict and late row: no event
        self.assertEqual(np.nansum(f[:, col(505)]), 0.0)                         # Arca is no event
        self.assertTrue(np.isnan(f[-1, col(505)]))                               # rows stop after 2021-04-10

    def test_manifest_entry_and_checks(self):
        e = next(x for x in self.manifest["fields"] if x["name"] == FIELD)
        spec = hold.HOLD_FIELDS[FIELD]
        self.assertEqual((e["formula_id"], e["units"], e["point_in_time"]), (spec["formula_id"], spec["units"], True))
        self.assertEqual(e["stage_manifest_sha256"], {"security_master": self.w.stage_sha})
        self.assertEqual(e["producer"]["module"], "research_fields_holdings.py")
        c = self.manifest["source_checks"]["holdings"]["listing_switch"]
        self.assertEqual((c["rows_on_role"], c["rows_sealed"]), (len([r for r in self.w.rows if r[0] in IDS]), 0))
        self.assertEqual((c["conflicts"], c["out_of_order_rows"]), (1, 2))      # 404's conflict; 404's and 303's late
        self.assertEqual(c["lines_with_an_up_switch"], 3)                        # 101, 202, 303
        self.assertEqual([x["name"] for x in self.manifest["fields"]], [FIELD])  # the carried mkt_ret leaves no trace

    def test_cli(self):
        with contextlib.redirect_stdout(io.StringIO()):
            tool.main(["--role", str(self.w.role), "--role-sha256", self.w.role_sha, "--output",
                       str(self.w.base / "cli"), "--fields", FIELD, "--security-master", str(self.w.stage),
                       "--security-master-sha256", self.w.stage_sha])
        m = json.loads((self.w.base / "cli" / "manifest.json").read_bytes())
        self.assertEqual(m["files"][f"{FIELD}.f64"], self.manifest["files"][f"{FIELD}.f64"])

    def test_refusals(self):
        with self.assertRaisesRegex(ValueError, "needs --security-master"):
            self.w.run("r1", security_master=None)
        with self.assertRaisesRegex(ValueError, "does not match --security-master-sha256"):
            self.w.run("r2", security_master_sha256="0" * 64)
        self.assertFalse((self.w.base / "r2" / "manifest.json").exists())


PROBE_DAY = dt.date(2020, 11, 2)        # t*: a session; every row at or after the mark of t*-1 is mutated


def probe_rows(rows, cut):
    """The world with every row available at or after ``cut`` changed: classes flipped toward NYSE and new switch rows
    added, one exactly at the cut."""
    out = []
    for sid, dd, av, c in rows:
        out.append((sid, dd, av, "NYSE" if av >= cut and c in ("NNM", "AMEX") else c))
    out.append((505, dt.date(2020, 10, 30), cut, "NYSE"))                         # exactly at the mark of t*-1
    out.append((101, dt.date(2020, 11, 1), cut + dt.timedelta(hours=1), "AMEX"))
    out.append((404, dt.date(2020, 11, 3), cut + dt.timedelta(days=2), "NYSE"))
    return out


class LookAhead(unittest.TestCase):
    def test_rows_after_the_mark_never_move_earlier_cells(self):
        t = SESSIONS.index(PROBE_DAY)
        cut = mark(SESSIONS[t - 1])
        with tempfile.TemporaryDirectory() as temp:
            a = World(Path(temp) / "a")
            b = World(Path(temp) / "b", probe_rows(a.rows, cut))
            a.run("o")
            b.run("o")
            fa, fb = a.field("o"), b.field("o")
            np.testing.assert_array_equal(fa[:t + 1], fb[:t + 1])
            self.assertFalse(np.array_equal(fa[t + 1:], fb[t + 1:], equal_nan=True))   # the mutation does bite later
            # the same probe catches a one-session look-ahead (marks of t instead of t-1)
            ahead = lambda days: days.astype(np.int64) * hold.DAY_NS + hold.MARK_NS   # noqa: E731
            with mock.patch.object(hold, "prev_marks", ahead):
                a.run("x")
                b.run("x")
            self.assertFalse(np.array_equal(a.field("x")[:t + 1], b.field("x")[:t + 1], equal_nan=True))


class Seal(unittest.TestCase):
    def test_reader_side_seal_drops_later_rows(self):
        seal = mark(dt.date(2021, 5, 1), 0)
        seal_ns = int(seal.timestamp()) * 1_000_000_000
        with tempfile.TemporaryDirectory() as temp:
            w = World(Path(temp) / "w")
            cut = World(Path(temp) / "cut", [r for r in world_rows() if r[2] < seal])
            w.run("open")
            with mock.patch.object(hold, "SEAL_NS", seal_ns):
                m = w.run("sealed")
            cut.run("cut")
            sealed = w.field("sealed")
            np.testing.assert_array_equal(sealed, cut.field("cut"))
            np.testing.assert_array_equal(sealed, oracle(w.rows, seal))
            self.assertFalse(np.array_equal(sealed, w.field("open"), equal_nan=True))   # the 2021-05-10 switch
            st = m["source_checks"]["holdings"]["listing_switch"]
            self.assertEqual(st["rows_sealed"], len([r for r in w.rows if r[2] >= seal]))


class Reuse(unittest.TestCase):
    def test_self_copy_pin_change_and_fingerprints(self):
        with tempfile.TemporaryDirectory() as temp:
            w = World(Path(temp))
            first = w.run("first")
            again = w.run("again", reuse=w.base / "first")
            self.assertEqual((again["reuse"]["reused"], again["reuse"]["computed"]), ([FIELD], []))
            self.assertEqual(again["files"], first["files"])
            e = next(x for x in again["fields"] if x["name"] == FIELD)
            # its stage pin and, like every holdings kind (FIX-2 review N-1; build_xsw drops rows at or after it), the seal
            self.assertEqual(e["reused_from"]["inputs"], {"security_master": w.stage_sha, "seal": hold.SEAL.isoformat()})
            # another security_master stage (pin) recomputes the field
            other = w.base / "security_master_2"
            rows = world_rows() + [(101, dt.date(2021, 8, 30), mark(dt.date(2021, 8, 30)), "NYSE")]
            sha2 = write_names(other, rows)
            third = w.run("third", reuse=w.base / "first", security_master=other, security_master_sha256=sha2)
            self.assertEqual(third["reuse"]["computed"], [FIELD])
        # producer fingerprints: the xsw closure is its own; an edit elsewhere leaves it, an edit in it moves only it
        src = tool.module_source(hold).replace(b"\r\n", b"\n")
        host = tool.builder_source()
        fps = tool.module_fingerprints(hold, src, host)
        self.assertEqual(sorted(fps), sorted(hold.PRODUCERS))
        for old, new, kinds in (
                (b"ven: np.ndarray) -> tuple:", b"ven: np.ndarray, _e=None) -> tuple:", {"xsw"}),
                (b"def build_xsw(ctx: Ctx, names, stage: Stage):", b"def build_xsw(ctx: Ctx, names, stage: Stage, _e=0):",
                 {"xsw"}),
                (b"XSW_WINDOW_DAYS = 365", b"XSW_WINDOW_DAYS = 366", {"xsw"}),
                (b"stage: Stage, names_stage: Stage):", b"stage: Stage, names_stage: Stage, _e=0):", {"regsho"}),
                (b"REGSHO_NAME_MAX_AGE_DAYS = 45", b"REGSHO_NAME_MAX_AGE_DAYS = 46", {"regsho"})):
            self.assertEqual(src.count(old), 1, old)
            edited = tool.module_fingerprints(hold, src.replace(old, new), host)
            self.assertEqual({k for k in fps if fps[k] != edited[k]}, kinds, old)


if __name__ == "__main__":
    unittest.main()
