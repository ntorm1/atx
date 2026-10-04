"""research_fields_mgr13f (platform v8 lane YDATA): a synthetic 13F stage and the builder's synthetic fixtures only.

Pure tests check the churn arithmetic (Gaspar-Massa-Matos minimum churn), the share-basis rule across a split and the
short-term tercile by hand. The field test builds a nine-quarter 13F world (2022q3 .. 2024q3; nine filers with fixed
trading intensities, a 2:1 split, an implied-price outlier, option / principal / zero-share rows, a filing after the
deadline, a 13F-NT notice) and checks every cell of ``stio_chg_q`` against the definition written out here with plain
loops, then probes look-ahead (shown to fail on a same-session-mark variant)."""
import contextlib
import datetime as dt
import hashlib
import io
import json
import math
from pathlib import Path
import tempfile
import unittest
import unittest.mock as mock

import numpy as np
import pyarrow as pa

import prepare_research_fields as tool
import prepare_research_fields_ydata as yreg
import research_fields_holdings as hold
import research_fields_mgr13f as mgr
import research_window as rw
import test_prepare_research_fields as base
import test_research_fields_holdings as thold

SESSIONS = thold.SESSIONS            # 2024-05-01 .. 2024-12-31 weekdays (the holdings fixture's role)
IDS = thold.IDS                      # 101 202 303 404 505
QS = [dt.date(2022, 9, 30), dt.date(2022, 12, 31), dt.date(2023, 3, 31), dt.date(2023, 6, 30), dt.date(2023, 9, 30),
      dt.date(2023, 12, 31), dt.date(2024, 3, 31), dt.date(2024, 6, 30), dt.date(2024, 9, 30)]
SECS = {"C101": 101, "C202": 202, "C303": 303, "C909": 909}       # 909 is mapped but not a role line
FILERS = [str(k) for k in range(1, 10)]
TAU = dict(zip(FILERS, (0.6, 0.5, 0.4, 0.2, 0.15, 0.1, 0.03, 0.02, 0.01)))   # trading intensity per filer
SPLIT_AFTER = dt.date(2023, 6, 30)   # 202 splits 2:1 between this quarter end and the next
OUTLIER = ("5", "C909", dt.date(2024, 3, 31))                     # value x 100: an implied-price outlier
EXIT = ("1", "C303", dt.date(2023, 12, 31))                       # filer 1 holds no 303 at this quarter end
FIELDS = list(yreg.FIELDS_MGR13F_DRAFT)
RECIPE = ["si_shares", "shares_out"] + FIELDS
CUT = dt.date(2024, 11, 18)          # Q3 (2024-09-30) becomes visible on 2024-11-19 (V = 11-15 22:00 UTC)


def deadline(q):
    return q + dt.timedelta(days=45)


def filing_date(filer, q):
    return deadline(q) if q == QS[-1] else deadline(q) - dt.timedelta(days=int(filer) % 3)


def holdings_world(seed=11):
    """{(filer, quarter): [(cusip, shares, value)]} of the effective original filings."""
    rng = np.random.default_rng(seed)
    price = {c: 40.0 + 10.0 * i for i, c in enumerate(list(SECS) + ["CUNM"])}
    shares = {(k, c): float(rng.integers(1000, 5000)) for k in FILERS for c in list(SECS) + ["CUNM"]}
    out = {}
    for q in QS:
        for c in price:
            price[c] *= math.exp(rng.normal(0.0, 0.1))
        if q == dt.date(2023, 9, 30):
            price["C202"] /= 2.0
            for k in FILERS:
                shares[(k, "C202")] *= 2.0
        for k in FILERS:
            rows = []
            for c in price:
                shares[(k, c)] = float(round(shares[(k, c)] * math.exp(rng.normal(0.0, TAU[k]))))
                if (k, c, q) == EXIT:
                    continue
                v = shares[(k, c)] * price[c]
                if (k, c, q) == OUTLIER:
                    v *= 100.0
                rows.append((c, shares[(k, c)], v))
            out[(k, q)] = rows
    return out


def write_stage(root: Path, world, mutate_after=None, seed=3):
    """The thirteenf stage of ``world``; ``mutate_after`` (an instant) rescales at random the shares and values of every
    filing available at or after it (the look-ahead probe)."""
    rng = np.random.default_rng(seed)
    f = {k: [] for k in ("accession", "filer_cik", "period_q", "filing_date", "submission_type", "amendment_type",
                         "available_at", "deadline", "source_period")}
    parts = {}

    def filing(acc, filer, q, fd, form, rows):
        avail = thold.instant(fd, 46)
        part = f"{fd.year}q{(fd.month - 1) // 3 + 1}"
        for k, v in zip(f, (acc, filer, q, fd, form, None, avail, deadline(q), part)):
            f[k].append(v)
        h = parts.setdefault(part, {k: [] for k in ("accession", "cusip", "shares", "sshprnamt_type", "put_call",
                                                     "value_usd")})
        for cusip, sh, val, typ, pc_ in rows:
            if mutate_after is not None and avail >= mutate_after:
                m = float(rng.uniform(0.3, 3.0))
                sh, val = sh * m, val * m
            for k, v in zip(h, (acc, cusip, float(sh), typ, pc_, float(val))):
                h[k].append(v)

    for (k, q), rows in world.items():
        extra = []
        if (k, q) == ("2", QS[-2]):     # rows every screen drops
            extra = [("C101", 5.0, 50.0, "SH", "CALL"), ("C101", 5.0, 50.0, "PRN", None), ("C101", 0.0, 50.0, "SH", None)]
        filing(f"{k}-{q}", k, q, filing_date(k, q), "13F-HR", [(c, s, v, "SH", None) for c, s, v in rows] + extra)
    filing("late-q3", "10", QS[-1], deadline(QS[-1]) + dt.timedelta(days=3), "13F-HR",
           [("C101", 1e9, 1e10, "SH", None)])                                     # after the deadline: never used
    filing("nt-q3", "11", QS[-1], deadline(QS[-1]), "13F-NT", [])                 # a notice: clock only
    tables = {"filings.parquet": pa.table({
        "accession": f["accession"], "filer_cik": f["filer_cik"], "period_q": pa.array(f["period_q"], pa.date32()),
        "filing_date": pa.array(f["filing_date"], pa.date32()), "submission_type": f["submission_type"],
        "amendment_type": pa.array(f["amendment_type"], pa.string()), "available_at": thold.ts(f["available_at"]),
        "deadline": pa.array(f["deadline"], pa.date32()), "source_period": f["source_period"]})}
    for part, h in parts.items():
        tables[f"parts/source={part}/holdings.parquet"] = pa.table({
            "accession": h["accession"], "cusip": h["cusip"], "shares": pa.array(h["shares"], pa.float64()),
            "sshprnamt_type": h["sshprnamt_type"], "put_call": pa.array(h["put_call"], pa.string()),
            "value_usd": pa.array(h["value_usd"], pa.float64())})
    tables["filing_checks.parquet"] = pa.table({"accession": ["none"], "unit_factor": [1.0]})
    cm = [(q, c, s) for q in QS for c, s in SECS.items()]
    tables["cusip_map_pit.parquet"] = pa.table({"period_q": pa.array([x[0] for x in cm], pa.date32()),
                                                "cusip": [x[1] for x in cm], "security_id": [x[2] for x in cm]})
    return thold.write_stage(root, "thirteenf", tables)


class Fixture(base.Fixture):
    def __init__(self, root: Path, mutate_after=None):
        self.base = root
        self.th = root / "TickerHistory3.parquet"
        base.write_tickerhistory(self.th)
        self.finra = root / "finra"
        base.write_finra(self.finra)
        self.lake = root / "lake"
        base.write_lake(self.lake)
        self.role = root / "role"
        self.role_sha = base.write_role(self.role, thold.sha(self.th), SESSIONS)
        self.stage = root / "thirteenf"
        self.stage_sha = write_stage(self.stage, holdings_world(), mutate_after)

    def run(self, out, **kw):
        kw.setdefault("module_options", {"mgr13f_stage": self.stage, "mgr13f_stage_sha256": self.stage_sha})
        return super().run(out, **kw)

    def field(self, out, name):
        return np.fromfile(self.base / out / f"{name}.f64", dtype="<f8").reshape(len(SESSIONS), len(IDS))


@contextlib.contextmanager
def registered():
    with mock.patch.dict(tool.ALL_FIELDS), mock.patch.object(tool, "FIELD_MODULES", list(tool.FIELD_MODULES)):
        yreg.register(vars(tool))
        yield


# ---------------------------------------------------------------------------------------------------------------------
# The definition written out with plain loops (the oracle)
# ---------------------------------------------------------------------------------------------------------------------

def median(xs):
    return float(np.median(np.array(xs, dtype=np.float64)))


def positions(world):
    """{q: ({(filer, sid): [shares, value]}, {sid: price})} after the screens of F13_ROWS_RULE."""
    out = {}
    for q in QS:
        rows = [(int(k), SECS[c], s, v) for (k, qq), rr in world.items() if qq == q for c, s, v in rr
                if c in SECS and s > 0]
        by = {}
        for _, sid, s, v in rows:
            if v > 0:
                by.setdefault(sid, []).append(v / s)
        med = {sid: median(p) for sid, p in by.items()}
        kept = [r for r in rows if not (r[3] > 0 and not (0.1 <= (r[3] / r[2]) / med[r[1]] <= 10.0))]
        pos, px = {}, {}
        for k, sid, s, v in kept:
            p = pos.setdefault((k, sid), [0.0, 0.0])
            p[0] += s
            p[1] += v
            if v > 0:
                px.setdefault(sid, []).append(v / s)
        out[q] = (pos, {sid: median(p) for sid, p in px.items()})
    return out


def oracle_churn(prev, cur):
    (pp, prp), (pc_, prc) = prev, cur
    fp, fc = {k for k, _ in pp}, {k for k, _ in pc_}
    both = fp & fc
    basis = {}
    for sid in {s for _, s in pp} | {s for _, s in pc_}:
        ratios = [pc_[(k, sid)][0] / pp[(k, sid)][0] for k in both if (k, sid) in pp and (k, sid) in pc_]
        a = 1.0
        if len(ratios) >= 5:
            s = median(ratios)
            if abs(math.log(s)) > math.log(1.2) and sid in prc and sid in prp and \
                    abs(math.log(s * prc[sid] / prp[sid])) <= math.log(1.5):
                a = s
        basis[sid] = a
    cr = {}
    for k in both:
        buy = sell = 0.0
        sids = {s for kk, s in pp if kk == k} | {s for kk, s in pc_ if kk == k}
        for sid in sids:
            a = basis[sid]
            d = pc_.get((k, sid), [0.0])[0] - a * pp.get((k, sid), [0.0])[0]
            x = prc[sid] if sid in prc else (prp[sid] / a if sid in prp else 0.0)
            buy += max(d, 0.0) * x
            sell += max(-d, 0.0) * x
        denom = (sum(v[1] for kk, v in pp.items() if kk[0] == k) + sum(v[1] for kk, v in pc_.items() if kk[0] == k)) / 2
        cr[k] = min(buy, sell) / denom if denom > 0 else math.nan
    return cr


def oracle(world, so):
    pos = positions(world)
    cr = {QS[i]: oracle_churn(pos[QS[i - 1]], pos[QS[i]]) for i in range(1, len(QS))}
    st = {}
    for i in range(4, len(QS)):
        window = [cr[QS[j]] for j in range(i - 3, i + 1)]
        fs = set(window[0]).intersection(*window[1:])
        mean = {k: sum(w[k] for w in window) / 4 for k in fs if all(math.isfinite(w[k]) for w in window)}
        m = len(mean)
        vals = list(mean.values())
        rank = {k: 1 + sum(x < v for x in vals) + (sum(x == v for x in vals) - 1) / 2 for k, v in mean.items()}
        st[QS[i]] = {k for k, r in rank.items() if r > 2.0 / 3.0 * m} if m >= 3 else None

    def stio_of(q, members, sid):
        p, _ = pos[q]
        if not any(s == sid for _, s in p):
            return math.nan
        t = max(i for i, d in enumerate(SESSIONS) if d <= q) if SESSIONS[0] <= q else -1
        if t < 0:
            return math.nan
        o = so[t, IDS.index(sid)]
        if not (math.isfinite(o) and o > 0):
            return math.nan
        v = sum(x[0] for (k, s), x in p.items() if s == sid and k in members) / o
        return v if v <= 2.0 else math.nan

    dstio = {}
    for i in range(5, len(QS)):
        q, q0 = QS[i], QS[i - 1]
        for sid in IDS:
            if st.get(q0) is None:
                dstio[(q, sid)] = math.nan
                continue
            dstio[(q, sid)] = stio_of(q, st[q0], sid) - stio_of(q0, st[q0], sid)
    vis = {q: dt.datetime.combine(deadline(q) if q == QS[-1] else max(filing_date(k, q) for k in FILERS),
                                  dt.time(0), dt.timezone.utc) + dt.timedelta(hours=46) for q in QS}
    out = np.full((len(SESSIONS), len(IDS)), np.nan)
    for t, d in enumerate(SESSIONS):
        if t == 0:
            continue
        mark = dt.datetime.combine(SESSIONS[t - 1], dt.time(22), dt.timezone.utc)
        for j, sid in enumerate(IDS):
            cands = [q for q in QS if vis[q] < mark and (d - q).days <= 150 and q <= d
                     and any(s == sid for _, s in pos[q][0])]
            if cands:
                out[t, j] = dstio.get((max(cands), sid), math.nan)
    return out, st, cr


class Pure(unittest.TestCase):
    def test_churn_by_hand(self):
        # filer 1: q-1 {101: 100 @ 10}; q {101: 50 @ 12, 202: 30 @ 5}: buys 150, sells 600, mean book (1000 + 750) / 2
        prev = mgr.quarter_positions([1], [101], [100], [1000])
        cur = mgr.quarter_positions([1, 1], [101, 202], [50, 30], [600, 150])
        F, cr, st = mgr.churn(prev, cur)
        self.assertEqual(F.tolist(), [1])
        self.assertAlmostEqual(cr[0], 150.0 / 875.0, places=15)
        self.assertEqual(st["basis_changes"], 0)
        # a filer only in one quarter has no churn; an exit is a sale at the last price restated
        prev = mgr.quarter_positions([1, 2], [101, 101], [100, 10], [1000, 100])
        cur = mgr.quarter_positions([1, 3], [202, 101], [10, 10], [100, 110])
        F, cr, _ = mgr.churn(prev, cur)
        self.assertEqual(F.tolist(), [1])
        self.assertAlmostEqual(cr[0], min(100.0, 100.0 * 11.0) / 550.0, places=15)

    def test_split_is_not_trading(self):
        # five holders keep their positions through a 2:1 split: shares double, the price halves -> churn 0
        filers = [1, 2, 3, 4, 5]
        prev = mgr.quarter_positions(filers + filers, [101] * 5 + [202] * 5, [100] * 5 + [50] * 5,
                                     [2000] * 5 + [500] * 5)
        cur = mgr.quarter_positions(filers + filers, [101] * 5 + [202] * 5, [200] * 5 + [60] * 5,
                                    [2100] * 5 + [600] * 5)
        F, cr, st = mgr.churn(prev, cur)
        self.assertEqual(st["basis_changes"], 1)
        np.testing.assert_allclose(cr, 0.0, atol=0)      # 202 only bought (+10 each): min(buys, sells) = 0
        # with four continuing holders the split is not called (BASIS_MIN_HOLDERS 5)
        _, _, st4 = mgr.churn(mgr.quarter_positions(filers[:4], [101] * 4, [100] * 4, [2000] * 4),
                              mgr.quarter_positions(filers[:4], [101] * 4, [200] * 4, [2100] * 4))
        self.assertEqual(st4["basis_changes"], 0)
        # the price does not offset the share ratio (value x 2.1): holders really doubled, no basis change
        _, _, st5 = mgr.churn(mgr.quarter_positions(filers, [101] * 5, [100] * 5, [2000] * 5),
                              mgr.quarter_positions(filers, [101] * 5, [200] * 5, [4200] * 5))
        self.assertEqual(st5["basis_changes"], 0)

    def test_short_term_tercile(self):
        filers = np.array([1, 2, 3, 4, 5, 6])
        hist = [(filers, np.array([0.1, 0.2, 0.3, 0.4, 0.5, 0.6]))] * 4
        self.assertEqual(mgr.short_term_set(hist).tolist(), [5, 6])
        tied = [(filers, np.array([0.1, 0.2, 0.3, 0.5, 0.5, 0.5]))] * 4
        self.assertEqual(mgr.short_term_set(tied).tolist(), [4, 5, 6])     # ranks 5, 5, 5 > 4
        self.assertIsNone(mgr.short_term_set(hist[:3]))
        self.assertIsNone(mgr.short_term_set(hist[:3] + [None]))
        gap = hist[:3] + [(filers[:2], np.array([0.1, 0.2]))]
        self.assertIsNone(mgr.short_term_set(gap))                          # m = 2 < 3
        np.testing.assert_array_equal(mgr.average_ranks(np.array([3.0, 1.0, 3.0, 2.0])), [3.5, 1.0, 3.5, 2.0])


class StioField(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._reg = registered()
        cls._reg.__enter__()
        cls.temp = tempfile.TemporaryDirectory()
        cls.fx = Fixture(Path(cls.temp.name))
        cls.manifest = cls.fx.run("fields", fields=RECIPE)
        cls.got = cls.fx.field("fields", "stio_chg_q")
        cls.so = cls.fx.field("fields", "shares_out")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()
        cls._reg.__exit__(None, None, None)

    def test_values_match_the_definition(self):
        want, st, cr = oracle(holdings_world(), self.so)
        self.assertTrue(np.array_equal(np.isnan(self.got), np.isnan(want)))
        ok = np.isfinite(want)
        self.assertGreater(int(ok.sum()), 60)
        np.testing.assert_allclose(self.got[ok], want[ok], rtol=1e-12, atol=1e-15)
        self.assertTrue(np.any(self.got[ok] != 0))
        # the classification: a tercile of the nine filers, never one of the three least active
        for q, s in st.items():
            if s is not None:
                self.assertEqual(len(s), 3, q)
                self.assertFalse(s & {7, 8, 9}, q)
        # the split quarter: buy-and-hold filer 9's churn stays small although its 202 shares doubled
        self.assertLess(cr[dt.date(2023, 9, 30)][9], 0.05)
        q3 = self.manifest["source_checks"]["mgr13f"]["thirteenf"]["per_quarter"]
        self.assertEqual(q3["2023-09-30"]["basis_changes"], 1)
        self.assertEqual(q3["2024-03-31"]["rows_price_outlier"], 1)

    def test_clock(self):
        t, j = SESSIONS.index(CUT), IDS.index(101)
        self.assertTrue(math.isnan(self.got[t, j]))           # still Q2 (its Q1 base is before the role: NaN)
        self.assertTrue(math.isfinite(self.got[t + 1, j]))    # Q3, visible from 2024-11-19
        e = {x["name"]: x for x in self.manifest["fields"]}["stio_chg_q"]
        self.assertEqual(e["producer"]["module"], "research_fields_mgr13f.py")
        self.assertEqual(e["depends_on"], ["shares_out"])
        self.assertEqual(e["stage_manifest_sha256"], self.fx.stage_sha)
        self.assertEqual(e["imported_code"], mgr.imported_code("y_stio"))
        clock = self.manifest["source_checks"]["mgr13f"]["thirteenf"]["quarter_visible_at"]
        self.assertEqual(clock["2024-09-30"], "2024-11-15T22:00:00.000000000Z")   # the late filing does not move V

    def test_point_in_time_probe(self):
        """Every holding available at or after the t-1 mark of CUT rescaled: rows <= CUT identical, later rows move;
        a variant reading the decision session's own mark moves row CUT (the probe has teeth)."""
        mark = dt.datetime.combine(SESSIONS[SESSIONS.index(CUT) - 1], dt.time(22), dt.timezone.utc)
        t = SESSIONS.index(CUT)
        with tempfile.TemporaryDirectory() as temp:
            moved = Fixture(Path(temp), mutate_after=mark)
            moved.run("f", fields=RECIPE)
            y = moved.field("f", "stio_chg_q")
            self.assertEqual(self.got[:t + 1].tobytes(), y[:t + 1].tobytes())
            self.assertNotEqual(self.got[t + 1:].tobytes(), y[t + 1:].tobytes())
            same_mark = lambda days: days.astype(np.int64) * hold.DAY_NS + hold.MARK_NS
            with mock.patch.object(hold, "prev_marks", same_mark):
                self.fx.run("leak-a", fields=RECIPE)
                moved.run("leak-b", fields=RECIPE)
            a, b = self.fx.field("leak-a", "stio_chg_q"), moved.field("leak-b", "stio_chg_q")
            self.assertNotEqual(a[:t + 1].tobytes(), b[:t + 1].tobytes())

    def test_refusals_and_seal(self):
        with self.assertRaisesRegex(ValueError, "need --mgr13f-stage"):
            self.fx.run("x1", fields=RECIPE, module_options={})
        self.assertFalse((self.fx.base / "x1").exists())
        with self.assertRaisesRegex(ValueError, "requires shares_out"):
            self.fx.run("x2", fields=FIELDS)
        with self.assertRaisesRegex(ValueError, "does not match"):
            self.fx.run("x3", fields=RECIPE, module_options={"mgr13f_stage": self.fx.stage,
                                                             "mgr13f_stage_sha256": "0" * 64})
        self.assertFalse((self.fx.base / "x3" / "manifest.json").exists())
        with mock.patch.object(tool, "SEAL", dt.date(2026, 1, 1)):
            with self.assertRaisesRegex(rw.SealError, "not research_window's"):
                self.fx.run("x4", fields=RECIPE)
        self.assertFalse((self.fx.base / "x4" / "manifest.json").exists())

    def test_reuse_and_cli(self):
        sha = hashlib.sha256((self.fx.base / "fields" / "manifest.json").read_bytes()).hexdigest()
        m = self.fx.run("again", fields=RECIPE, reuse=self.fx.base / "fields", reuse_sha256=sha)
        self.assertIn("stio_chg_q", m["reuse"]["reused"])
        self.assertEqual((self.fx.base / "again" / "stio_chg_q.f64").read_bytes(),
                         (self.fx.base / "fields" / "stio_chg_q.f64").read_bytes())
        argv = ["--role", str(self.fx.role), "--role-sha256", self.fx.role_sha, "--output", str(self.fx.base / "cli"),
                "--fields", ",".join(RECIPE), "--finra", str(self.fx.finra), "--tickerhistory", str(self.fx.th),
                "--lake", str(self.fx.lake), "--mgr13f-stage", str(self.fx.stage),
                "--mgr13f-stage-sha256", self.fx.stage_sha]
        with contextlib.redirect_stdout(io.StringIO()):
            tool.main(argv)
        self.assertEqual((self.fx.base / "cli" / "stio_chg_q.f64").read_bytes(),
                         (self.fx.base / "fields" / "stio_chg_q.f64").read_bytes())

    def test_plain_builder_does_not_register_the_module(self):
        self.assertTrue(any(type(m).__module__ == "research_fields_mgr13f" for m in tool.FIELD_MODULES))
        self._reg.__exit__(None, None, None)
        try:
            self.assertNotIn("stio_chg_q", tool.ALL_FIELDS)
            self.assertFalse(any(type(m).__module__ == "research_fields_mgr13f" for m in tool.FIELD_MODULES))
        finally:
            type(self)._reg = registered()
            self._reg.__enter__()


if __name__ == "__main__":
    unittest.main()
