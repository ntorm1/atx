"""research_fields_xdata (platform v8 lane XDATA): synthetic vendor source and role only, no real archive access.

The world: a TickerHistory3-like parquet (dates mixed in every row group) from 2012-06 to 2019-05 for seven role lines
with known dividend schedules (quarterly, monthly, annual, a non-payer, an initiation, a semiannual payer with a
special above the band and a step below it, a factor step with no raw drop, splits) and two vendor lines off the role
axis: SPY (the market line, with an ATM IV path, out-of-domain prints, a missing print and a duplicated session) and
QQQ. The role is the NYSE sessions 2018-07-02 .. 2019-04-30 projected from it. Every field is built through the
builder's run() with the draft module registered, checked against definitions written out here independently, and put
through a look-ahead probe that is shown to fail on leaky variants of the module."""
import contextlib
import datetime as dt
import hashlib
import io
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import unittest.mock as mock

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import prepare_research_fields as tool
import prepare_research_fields_xdata as xreg
import research_fields_sec as sec
import research_fields_xdata as xdata
import research_window as rw

EPOCH = dt.date(1970, 1, 1)
DAY_NS = 86_400_000_000_000
TOOLS = Path(__file__).resolve().parent
TH_FIRST, ROLE_FIRST, ROLE_LAST, TH_LAST = dt.date(2012, 6, 1), dt.date(2018, 7, 2), dt.date(2019, 4, 30), \
    dt.date(2019, 5, 31)
ROLE_IDS = [11, 22, 33, 44, 55, 66, 77]
SPY, QQQ = 900, 901
TICKERS = {11: "AAA", 22: "BBB", 33: "CCC", 44: "DDD", 55: "EEE", 66: "FFF", 77: "GGG", SPY: "SPY", QQQ: "QQQ"}
FIELDS = list(xreg.FIELDS_XDATA_DRAFT)
# dividend schedules: (months, yield, first ex-date allowed); the ex-date is the first session on or after day 15
SCHEDULE = {11: ((2, 5, 8, 11), 0.008, None), 22: ((3, 6, 9, 12), 0.006, None), 33: (tuple(range(1, 13)), 0.003, None),
            44: ((4,), 0.02, None), 55: ((), 0.0, None), 66: ((1, 4, 7, 10), 0.007, dt.date(2018, 7, 1)),
            77: ((3, 6, 9, 12), 0.012, None), SPY: ((3, 6, 9, 12), 0.004, None), QQQ: ((), 0.0, None)}
SPECIAL = {77: [(dt.date(2018, 3, 15), 0.06), (dt.date(2017, 9, 15), 0.00005)]}   # above the band; below 1 bp
JUMP = (44, dt.date(2017, 10, 16), 0.02)       # a factor step with no raw drop: a jump cell, never an ex-date
SPLITS = {22: [(dt.date(2016, 6, 1), 2), (dt.date(2019, 2, 1), 3)]}
LISTED = {55: dt.date(2016, 3, 1)}
GAP_IDS = {11: 0.02}                           # line 11 has no row on 2% of sessions
GAMMA = {11: -0.5, 22: 0.3, 33: 0.0, 44: -1.0, 55: 0.8, 66: -0.2, 77: 0.5, SPY: 0.0, QQQ: 0.4}
SPY_IV_PRINTS = {dt.date(2018, 9, 17): 7.5, dt.date(2018, 11, 15): 0.0, dt.date(2018, 12, 10): None}
SPY_DUP = dt.date(2018, 10, 1)
SEAL_ROWS = [(dt.date(2025, 2, 3), 22), (dt.date(2025, 2, 3), SPY)]            # after the superseded seal
LATE_ROWS = [(dt.date(2024, 3, 1), 11), (dt.date(2024, 3, 1), SPY)]           # sealed under the repository window
CUT = 120                                      # the probe's mutation session (role row)


def day(d):
    return (d - EPOCH).days


def sessions(first, last):
    return [EPOCH + dt.timedelta(days=int(x)) for x in sec.nyse_sessions(first, last)]


def month_id(d: int) -> int:
    x = EPOCH + dt.timedelta(days=int(d))
    return x.year * 12 + x.month - 1


def ex_dates(cal, sid):
    months, _, start = SCHEDULE[sid]
    out, seen = set(), set()
    for d in cal:
        key = (d.year, d.month)
        if d.month in months and d.day >= 15 and key not in seen and (start is None or d >= start):
            seen.add(key)
            out.add(d)
    return out


def world(seed=20261002, tickers=TICKERS):
    """Vendor rows {column: list}."""
    rng = np.random.default_rng(seed)
    cal = sessions(TH_FIRST, TH_LAST)
    mkt = rng.normal(0.0003, 0.01, len(cal))
    iv, x = np.empty(len(cal)), 0.16
    for i in range(len(cal)):
        x = min(max(x - 0.6 * mkt[i] + rng.normal(0.0, 0.006), 0.06), 0.8)
        iv[i] = x
    div = np.concatenate(([0.0], np.diff(iv)))
    rows = {k: [] for k in ("tradingDate", "securityID", "ticker_tk", "close", "volume", "cumulReturnFactor",
                            "atmCenI_21d")}
    for n, sid in enumerate(ROLE_IDS + [SPY, QQQ]):
        beta = 1.0 if sid == SPY else 0.6 + 0.15 * n
        lp, f, k_split = math.log(20.0 + 5 * n), 1.0, 1.0
        exd, (_, y, _) = ex_dates(cal, sid), SCHEDULE[sid]
        special = dict(SPECIAL.get(sid, []))
        miss = GAP_IDS.get(sid, 0.0)
        for i, d in enumerate(cal):
            quiet = False
            if d in exd:                          # ex-date: the factor steps up, the raw close drops by the yield
                f /= 1.0 - y
                lp += math.log(1.0 - y)
                quiet = True
            for when, ys in list(special.items()):
                if d >= when:
                    f /= 1.0 - ys
                    lp += math.log(1.0 - ys)
                    quiet = True
                    del special[when]
            if (sid, d) == JUMP[:2]:
                f /= 1.0 - JUMP[2]                # no raw move
                quiet = True
            for when, k in SPLITS.get(sid, []):
                if d == when:
                    k_split *= k
                    f *= k
            if not quiet:                        # an event session carries only the event (deterministic labels)
                lp += (mkt[i] if sid == SPY else beta * mkt[i] + GAMMA[sid] * div[i] + rng.normal(0.0, 0.015))
            if sid in LISTED and d < LISTED[sid]:
                continue
            if miss and rng.random() < miss:
                continue
            v = float(iv[i]) if sid == SPY else 0.3
            if sid == SPY and d in SPY_IV_PRINTS:
                v = SPY_IV_PRINTS[d]
            for key, val in (("tradingDate", d), ("securityID", sid), ("ticker_tk", tickers[sid]),
                             ("close", math.exp(lp) / k_split), ("volume", float(rng.integers(10_000, 90_000))),
                             ("cumulReturnFactor", f), ("atmCenI_21d", v)):
                rows[key].append(val)
            if sid == SPY and d == SPY_DUP:      # a duplicated SPY session: quarantined
                for key in rows:
                    rows[key].append(rows[key][-1] if key != "close" else rows[key][-1] * 1.01)
    extra = [(dt.date(2017, 5, 3), 33), (dt.date(2017, 5, 6), 11)] + SEAL_ROWS + LATE_ROWS   # dup key; a Saturday
    for d, sid in extra:
        for key, x in (("tradingDate", d), ("securityID", sid), ("ticker_tk", tickers[sid]), ("close", 99.0),
                       ("volume", 1e4), ("cumulReturnFactor", 1.0), ("atmCenI_21d", 0.2)):
            rows[key].append(x)
    return rows


TYPES = {"tradingDate": pa.date32(), "securityID": pa.int64(), "ticker_tk": pa.string(), "close": pa.float32(),
         "volume": pa.float64(), "cumulReturnFactor": pa.float64(), "atmCenI_21d": pa.float32()}


def write_source(path, rows, drop=()):
    order = np.random.default_rng(7).permutation(len(rows["securityID"]))     # dates mixed in every row group
    pq.write_table(pa.table({k: pa.array([rows[k][i] for i in order], t) for k, t in TYPES.items() if k not in drop}),
                   path, row_group_size=1500)
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def observations(rows, seal=dt.date(2025, 1, 1)):
    """{(sid, epoch day): (raw f32 as f64, F, volume, iv f32 as f64 in domain else nan)} of the unique, valid,
    on-calendar rows before the seal."""
    seen, keep = {}, {}
    for i, (d, s) in enumerate(zip(rows["tradingDate"], rows["securityID"])):
        seen[(s, day(d))] = seen.get((s, day(d)), 0) + 1
        keep[(s, day(d))] = i
    cal = set(int(x) for x in sec.nyse_sessions(TH_FIRST, TH_LAST))
    lo, hi = np.float32(0.02), np.float32(5.0)
    out = {}
    for key, i in keep.items():
        raw, f, v, iv = float(np.float32(rows["close"][i])), rows["cumulReturnFactor"][i], rows["volume"][i], \
            rows["atmCenI_21d"][i]
        if seen[key] != 1 or key[1] not in cal or key[1] >= day(seal):
            continue
        if raw > 0 and f > 0 and v >= 0:
            iv32 = np.float32(iv) if iv is not None else np.float32(np.nan)
            out[key] = (raw, f, v, float(iv32) if (np.isfinite(iv32) and lo <= iv32 <= hi) else math.nan)
    return out


def write_role(root, rows, source_sha):
    root.mkdir(parents=True)
    obs = observations(rows)
    days = [day(d) for d in sessions(ROLE_FIRST, ROLE_LAST)]
    nd, n = len(days), len(ROLE_IDS)
    close, raw, volume = (np.full((nd, n), np.nan) for _ in range(3))
    present = np.zeros((nd, n), dtype="u1")
    for t, d in enumerate(days):
        for j, sid in enumerate(ROLE_IDS):
            o = obs.get((sid, d))
            if o:
                raw[t, j], close[t, j], volume[t, j], present[t, j] = o[0], o[0] * o[1], o[2], 1
    member = present.copy()
    member[5:9, 0] = 0
    blobs = {"sessions.i64": np.array([d * DAY_NS for d in days], dtype="<i8").tobytes(),
             "ids.u64": np.array(ROLE_IDS, dtype="<u8").tobytes(), "member.u8": member.tobytes(),
             "close.f64": close.astype("<f8").tobytes(), "raw_close.f64": raw.astype("<f8").tobytes(),
             "present.u8": present.tobytes(), "volume.f64": volume.astype("<f8").tobytes()}
    files = {}
    for name, blob in blobs.items():
        (root / name).write_bytes(blob)
        files[name] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete", "dates": nd, "instruments": n,
                "instrument_namespace": "spiderrock.securityID", "score_begin": 20, "score_end": nd,
                "source_sha256": source_sha, "files": files, "clock_recipe": "modeled-session+22h-mark+23h-decision-v1"}
    (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return hashlib.sha256((root / "manifest.json").read_bytes()).hexdigest()


@contextlib.contextmanager
def registered():
    """The draft module registered into the builder for the duration (undone on exit: every other test module sees
    the plain builder)."""
    with mock.patch.dict(tool.ALL_FIELDS), mock.patch.object(tool, "FIELD_MODULES", list(tool.FIELD_MODULES)):
        xreg.register(vars(tool))
        yield


class Case:
    def __init__(self, base: Path, rows, name="w", drop=()):
        self.base, self.rows = base, rows
        self.src = base / f"{name}.parquet"
        sha = write_source(self.src, rows, drop)
        self.role = base / f"{name}-role"
        self.role_sha = write_role(self.role, rows, sha)
        self.days = [day(d) for d in sessions(ROLE_FIRST, ROLE_LAST)]

    def run(self, out, fields, **kw):
        kw.setdefault("module_options", {"price_source": self.src})
        with contextlib.redirect_stdout(io.StringIO()):
            return tool.run(self.role, self.role_sha, self.base / out, fields, **kw)

    def field(self, out, name):
        return np.fromfile(self.base / out / f"{name}.f64", dtype="<f8").reshape(len(self.days), len(ROLE_IDS))


def calendar():
    return [int(x) for x in sec.nyse_sessions(TH_FIRST - dt.timedelta(days=800), ROLE_LAST)]


# ---- independent definitions -------------------------------------------------------------------------------------

def ref_events(obs, sid):
    ds = sorted(d for (s, d) in obs if s == sid)
    out = []
    for p, s in zip(ds, ds[1:]):
        (cp, fp, _, _), (cs, fs, _, _) = obs[(sid, p)], obs[(sid, s)]
        y = 1.0 - fp / fs
        sf, r = math.log(fs) - math.log(fp), math.log(cs) - math.log(cp)
        jump = abs(sf) > 0.01 and abs(r + sf) > abs(r) + 0.01
        if 1e-4 <= y <= 0.04 and not jump:
            out.append(s)
    return out


def ref_div(obs, cal, ti, sid, events):
    m = month_id(cal[ti])
    paid = {month_id(d) for d in events[sid] if d <= cal[ti - 1]}
    first12 = min(d for d in cal if month_id(d) == m - 12)
    if min(d for (s, d) in obs if s == sid) > first12:
        return math.nan
    n_paid = sum(1 for k in range(1, 13) if m - k in paid)
    if n_paid < 3 or n_paid > 6:
        return math.nan
    return 1.0 if any(m - k in paid for k in (3, 6, 9, 12)) else 0.0


def ref_line_ret(obs, sid, s, p):
    a, b = obs.get((sid, s)), obs.get((sid, p))
    if not (a and b):
        return math.nan
    la = math.log(a[0] * a[1]) - math.log(b[0] * b[1])
    lr = math.log(a[0]) - math.log(b[0])
    if abs(la) > 1.5 or abs(la) > abs(lr) + 0.10:
        return math.nan
    return (a[0] * a[1]) / (b[0] * b[1]) - 1.0


def ref_spy(obs, s, p):
    a, b = obs.get((SPY, s)), obs.get((SPY, p))
    if not (a and b):
        return math.nan, math.nan
    r, sf = math.log(a[0]) - math.log(b[0]), math.log(a[1]) - math.log(b[1])
    x = r + sf
    ok = abs(x) <= 1.5 and abs(x) <= abs(r) + 0.10 and not (abs(sf) > 0.01 and abs(x) > abs(r) + 0.01)
    return (math.expm1(x) if ok else math.nan), a[3] - b[3]


def ref_dvol(obs, cal, ti, sid):
    xs = []
    for i in range(ti - 21, ti):
        r = ref_line_ret(obs, sid, cal[i], cal[i - 1])
        m, dv = ref_spy(obs, cal[i], cal[i - 1])
        if all(math.isfinite(v) for v in (r, m, dv)):
            xs.append((r, m, dv))
    if len(xs) < 17:
        return math.nan
    a = np.array(xs)
    return float(np.linalg.lstsq(np.column_stack((np.ones(len(a)), a[:, 1], a[:, 2])), a[:, 0], rcond=None)[0][2])


def ref_season(obs, cal, ti, sid):
    rs = []
    for k in (2, 3, 4, 5):
        b, a = cal[ti - 252 * k], cal[ti - 252 * k + 21]
        pa_, pb = obs.get((sid, a)), obs.get((sid, b))
        if not (pa_ and pb):
            continue
        la = math.log(pa_[0] * pa_[1]) - math.log(pb[0] * pb[1])
        lr = math.log(pa_[0]) - math.log(pb[0])
        if abs(la) <= abs(lr) + 0.10:
            rs.append((pa_[0] * pa_[1]) / (pb[0] * pb[1]) - 1.0)
    return sum(rs) / len(rs) if len(rs) >= 3 else math.nan


def mutated(rows, cut_day, seed=5):
    """Every vendor row dated on or after ``cut_day`` (every line, SPY and QQQ included) moved at random."""
    rng = np.random.default_rng(seed)
    late = {k: list(v) for k, v in rows.items()}
    for i, d in enumerate(late["tradingDate"]):
        if day(d) >= cut_day:
            late["close"][i] *= float(rng.uniform(0.5, 1.5))
            late["cumulReturnFactor"][i] *= float(rng.uniform(0.9, 1.1))
            late["volume"][i] *= float(rng.uniform(0.1, 10.0))
            if late["atmCenI_21d"][i] is not None:
                late["atmCenI_21d"][i] *= float(rng.uniform(0.7, 1.3))
    return late


class XdataFields(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)
        self._reg = registered()
        self._reg.__enter__()

    def tearDown(self):
        self._reg.__exit__(None, None, None)
        self._tmp.cleanup()

    def probe(self, name, cut_row, mutate_day, patches=()):
        """The look-ahead probe: build ``name`` on the world and on the world with every row dated on or after
        ``mutate_day`` moved; returns (rows <= cut_row bit-identical, rows after cut_row differ)."""
        rows = world()
        with contextlib.ExitStack() as stack:
            for target, attr, value in patches:
                stack.enter_context(mock.patch.object(target, attr, value))
            tag = f"{name}-{len(list(self.base.iterdir()))}"
            a = Case(self.base, rows, tag + "a")
            a.run(tag + "fa", [name])
            b = Case(self.base, mutated(rows, mutate_day), tag + "b")
            b.run(tag + "fb", [name])
        x, y = a.field(tag + "fa", name), b.field(tag + "fb", name)
        return x[:cut_row + 1].tobytes() == y[:cut_row + 1].tobytes(), x[cut_row + 1:].tobytes() != y[cut_row + 1:].tobytes()

    def test_values_match_definitions(self):
        rows = world()
        case = Case(self.base, rows)
        m = case.run("f", FIELDS)
        obs, cal = observations(rows), calendar()
        events = {sid: ref_events(obs, sid) for sid in ROLE_IDS}
        got = {x: case.field("f", x) for x in FIELDS}
        checked = {x: 0 for x in FIELDS}
        for t in range(0, len(case.days), 3):
            ti = cal.index(case.days[t])
            for j, sid in enumerate(ROLE_IDS):
                for name, want in (("div_month_pred", ref_div(obs, cal, ti, sid, events)),
                                   ("beta_dvol_21", ref_dvol(obs, cal, ti, sid)),
                                   ("season_y2_5", ref_season(obs, cal, ti, sid))):
                    v = got[name][t, j]
                    if math.isnan(want):
                        self.assertTrue(math.isnan(v), (name, t, sid, v))
                    else:
                        checked[name] += 1
                        self.assertTrue(math.isclose(v, want, rel_tol=1e-7, abs_tol=1e-10), (name, t, sid, v, want))
        self.assertTrue(all(c >= 150 for c in checked.values()), checked)
        div = got["div_month_pred"]
        col = {sid: j for j, sid in enumerate(ROLE_IDS)}
        for sid in (33, 44, 55):                                  # monthly, annual, non-payer
            self.assertTrue(np.isnan(div[:, col[sid]]).all(), sid)
        months = [month_id(d) for d in case.days]
        third = month_id(day(dt.date(2019, 1, 16)))               # 66's third ex-date month (initiated 2018-07)
        self.assertTrue(np.isnan(div[[t for t, mm in enumerate(months) if mm <= third], col[66]]).all())
        self.assertTrue((div[[t for t, mm in enumerate(months) if mm == third + 1], col[66]] == 0.0).all())
        self.assertTrue((div[[t for t, mm in enumerate(months) if mm == third + 3], col[66]] == 1.0).all())
        self.assertEqual(set(np.unique(div[np.isfinite(div)])), {0.0, 1.0})
        for sid in (11, 22, 77):                                  # quarterly payers: every role month is defined
            self.assertTrue(np.isfinite(div[:, col[sid]]).all(), sid)
        # the special above the band, the step below 1 bp and the jump cell are no ex-dates
        led = m["source_checks"]["xdata"]
        self.assertNotIn(day(dt.date(2018, 3, 15)), events[77])
        self.assertNotIn(day(dt.date(2017, 10, 16)), events[44])
        e = {x["name"]: x for x in m["fields"]}
        self.assertGreaterEqual(e["div_month_pred"]["ledger"]["jump_steps_excluded"], 1)
        self.assertGreaterEqual(e["div_month_pred"]["ledger"]["steps_above_max_yield"], 3)   # 22's splits and 77's 6%
        st = led["vol_line"]
        self.assertEqual((st["security_id"], st["duplicate_sessions_quarantined"], st["iv_out_of_domain"]),
                         (SPY, 1, 2))
        self.assertEqual(st["rows_on_or_after_seal_skipped"], 1)
        self.assertEqual(led["source"]["rows_on_or_after_seal_skipped"], 2)
        for name in FIELDS:
            self.assertTrue(e[name]["point_in_time"])
            self.assertEqual(e[name]["producer"]["module"], "research_fields_xdata.py")
            self.assertEqual(e[name]["formula_sha256"], tool.formula_id(name, tool.spec_definition(name, 1)))
            self.assertEqual(e[name]["lag_sessions"], 1)
            self.assertEqual(e[name]["imported_code"], xdata.imported_code(xdata.FIELDS[name]["group"]))
            self.assertEqual(e[name]["session_calendar"], xdata.price.session_calendar())

    def test_point_in_time_probe(self):
        """Rows <= t never move when every vendor row dated on or after session t moves (div_month_pred and
        beta_dvol_21 do move after it); season_y2_5 reads nothing after t-483, so its probe cuts there."""
        days = [day(d) for d in sessions(ROLE_FIRST, ROLE_LAST)]
        for name in ("div_month_pred", "beta_dvol_21"):
            same, moved = self.probe(name, CUT, days[CUT])
            self.assertTrue(same, name)
            self.assertTrue(moved, name)
        cal = calendar()
        early = cal[cal.index(days[CUT]) - 483 + 1]           # the first session row CUT does not read
        same, moved = self.probe("season_y2_5", CUT, early)
        self.assertTrue(same and moved)

    def test_probe_fails_on_leaky_variants(self):
        """The probe has teeth: each field built with a look-ahead (the same-session window, or the current month /
        year read 21 sessions ahead) moves at or before the cut, so the probe above would fail on it."""
        days = [day(d) for d in sessions(ROLE_FIRST, ROLE_LAST)]
        early_jan = days.index(day(dt.date(2019, 1, 3)))   # a month where 11, 22 and 77 have no predicted ex-date
        leaks = {"beta_dvol_21": (CUT, [(xdata, "LAG_SESSIONS", 0)]),
                 "div_month_pred": (early_jan, [(xdata, "LAG_SESSIONS", -21),
                                                (xdata, "DIV_PRED_MONTHS", (0, 3, 6, 9, 12))]),
                 "season_y2_5": (CUT, [(xdata, "LAG_SESSIONS", -21), (xdata, "SEASON_YEARS", (0, 2, 3, 4, 5))])}
        for name, (cut, patches) in leaks.items():
            same, _ = self.probe(name, cut, days[cut], patches)
            self.assertFalse(same, name)
            same, _ = self.probe(name, cut, days[cut])          # the module as written passes the same probe
            self.assertTrue(same, name)

    def test_seal_and_refusals(self):
        rows = world()
        case = Case(self.base, rows)
        with self.assertRaisesRegex(ValueError, "need --price-source"):
            case.run("x1", FIELDS, module_options={})
        self.assertFalse((self.base / "x1").exists())
        with mock.patch.object(tool, "SEAL", dt.date(2026, 1, 1)):       # the builder's seal is not research_window's
            with self.assertRaisesRegex(rw.SealError, "not research_window's"):
                case.run("x2", ["season_y2_5"])
        self.assertFalse((self.base / "x2" / "manifest.json").exists())
        noiv = Case(self.base, rows, "noiv", drop=("atmCenI_21d",))
        with self.assertRaisesRegex(ValueError, "column atmCenI_21d is missing"):
            noiv.run("x3", ["beta_dvol_21"])
        self.assertFalse((self.base / "x3").exists())
        noiv.run("ok", ["div_month_pred", "season_y2_5"])               # the other fields do not need SPY's IV
        two = Case(self.base, world(tickers={**TICKERS, QQQ: "SPY"}), "two")
        with self.assertRaisesRegex(ValueError, "maps to 2 vendor securityIDs"):
            two.run("x4", ["beta_dvol_21"])
        self.assertFalse((self.base / "x4" / "manifest.json").exists())
        # the CLI entry: same payloads as the API run
        argv = ["--role", str(case.role), "--role-sha256", case.role_sha, "--output", str(self.base / "cli"),
                "--fields", ",".join(FIELDS), "--price-source", str(case.src)]
        with contextlib.redirect_stdout(io.StringIO()):
            tool.main(argv)
        case.run("api", FIELDS)
        for name in FIELDS:
            self.assertEqual((self.base / "cli" / f"{name}.f64").read_bytes(),
                             (self.base / "api" / f"{name}.f64").read_bytes())

    def test_repository_window_fresh_interpreter(self):
        """Under the repository window (seal from research_window.json, never this harness's superseded one) a fresh
        interpreter drops the 2024 rows as sealed and writes the same payloads."""
        case = Case(self.base, world())
        case.run("here", FIELDS)
        script = (
            "import json, sys\n"
            f"sys.path.insert(0, {str(TOOLS)!r})\n"
            "from pathlib import Path\n"
            "import contextlib, io\n"
            "import prepare_research_fields as tool, prepare_research_fields_xdata as xreg, research_window as rw\n"
            "xreg.register(vars(tool))\n"
            "with contextlib.redirect_stdout(io.StringIO()):\n"
            f"    m = tool.run(Path({str(case.role)!r}), {case.role_sha!r}, Path({str(self.base / 'there')!r}), "
            f"{FIELDS!r}, module_options={{'price_source': Path({str(case.src)!r})}})\n"
            "x = m['source_checks']['xdata']\n"
            "print(json.dumps({'seal': rw.SEAL_DATE, 'manifest_seal': m['seal']['exclusive_end'],"
            " 'vol': x['vol_line']['rows_on_or_after_seal_skipped'],"
            " 'src': x['source']['rows_on_or_after_seal_skipped']}))\n")
        out = subprocess.run([sys.executable, "-c", script], cwd=str(self.base), capture_output=True, text=True,
                             timeout=300)
        self.assertEqual(out.returncode, 0, out.stderr[-2000:])
        got = json.loads(out.stdout.strip().splitlines()[-1])
        repo = rw.load()["seal_begin"]
        self.assertEqual((got["seal"], got["manifest_seal"]), (repo, repo))
        self.assertEqual((got["vol"], got["src"]), (2, 4))             # + the 2024 SPY row; + both 2024 rows
        for name in FIELDS:
            self.assertEqual((self.base / "there" / f"{name}.f64").read_bytes(),
                             (self.base / "here" / f"{name}.f64").read_bytes(), name)

    def test_reuse(self):
        case = Case(self.base, world())
        full = case.run("full", FIELDS)
        again = case.run("again", FIELDS, reuse=self.base / "full")
        self.assertEqual((again["reuse"]["reused"], again["reuse"]["computed"]), (FIELDS, []))
        self.assertEqual(again["files"], full["files"])
        for name in FIELDS:
            e = dict(next(x for x in again["fields"] if x["name"] == name))
            rec = e.pop("reused_from")
            self.assertEqual(e, next(x for x in full["fields"] if x["name"] == name), name)
            self.assertEqual(rec["inputs"], {"session_calendar": xdata.price.session_calendar(),
                                             "imported_code": xdata.imported_code(xdata.FIELDS[name]["group"])})
        case.run("part", ["season_y2_5"])
        mixed = case.run("mixed", FIELDS, reuse=self.base / "part")
        self.assertEqual((mixed["reuse"]["reused"], mixed["reuse"]["computed"]),
                         (["season_y2_5"], ["div_month_pred", "beta_dvol_21"]))
        self.assertEqual(mixed["files"], full["files"])
        # an edit of the code imported from research_fields_price.py recomputes every field (input pin)
        real = xdata.imported_code
        edited = lambda group, *a, **k: {**real(group), "sha256": "0" * 64}   # noqa: E731
        with mock.patch.object(xdata, "imported_code", edited):
            one = case.run("one", FIELDS, reuse=self.base / "full")
        self.assertEqual(one["reuse"]["computed"], FIELDS)
        self.assertEqual(one["files"], full["files"])
        # the imported closure really follows research_fields_price.py: an edit of source_panel moves it
        src = Path(xdata.price.__file__).read_bytes().replace(b"\r\n", b"\n")
        head = b"def source_panel(h, path: Path, verified, role, budget, *, pre_sessions: int, lookback_days: int,"
        self.assertEqual(src.count(head), 1)
        moved = src.replace(head, head.replace(b"budget, *", b"budget, _edited=None, *"))
        self.assertNotEqual(xdata.imported_code("x_div", source=moved)["sha256"], real("x_div")["sha256"])
        for name in xdata.FIELDS:
            self.assertEqual(xdata.producer_group(name), xdata.FIELDS[name]["group"])
            self.assertIs(xdata.field_spec(name), xdata.FIELDS[name])

    def test_registration_is_opt_in(self):
        self._reg.__exit__(None, None, None)          # the plain builder
        try:
            self.assertFalse(set(FIELDS) & set(tool.ALL_FIELDS))
            self.assertFalse([m for m in tool.FIELD_MODULES if type(m).__module__ == xdata.__name__])
            self.assertEqual(tuple(xdata.FIELDS), xreg.FIELDS_XDATA_DRAFT)
            with registered():
                names = list(tool.ALL_FIELDS)
                self.assertEqual(names[-len(FIELDS):], FIELDS)            # after every existing field
                before = len(tool.FIELD_MODULES)
                xreg.register(vars(tool))                                 # idempotent
                self.assertEqual(len(tool.FIELD_MODULES), before)
            self.assertFalse(set(FIELDS) & set(tool.ALL_FIELDS))
        finally:
            self._reg = registered()
            self._reg.__enter__()


if __name__ == "__main__":
    unittest.main()
