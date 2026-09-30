"""research_fields_price (platform v8 F-1): synthetic vendor source and role only, no real archive access.

The world: a TickerHistory3-like parquet (dates mixed in every row group) from 2012-06 to 2019-05 for six role lines
and two vendor lines off the role axis, and a role of the NYSE sessions 2018-07-02 .. 2019-04-30 projected from it.
Every field is built through the builder's run() (the FIELD_MODULES hook) and checked against definitions written
out here independently: the point-in-time property, split invariance, the chained factor and a numpy coskewness."""
import contextlib
import datetime as dt
import hashlib
import io
import json
import math
from pathlib import Path
import tempfile
import unittest
import unittest.mock

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import prepare_research_fields as tool
import research_fields_price as price
import research_fields_sec as sec

EPOCH = dt.date(1970, 1, 1)
DAY_NS = 86_400_000_000_000
TH_FIRST, ROLE_FIRST, ROLE_LAST, TH_LAST = dt.date(2012, 6, 1), dt.date(2018, 7, 2), dt.date(2019, 4, 30), \
    dt.date(2019, 5, 31)
ROLE_IDS = [11, 22, 33, 44, 55, 66]
OTHER_IDS = [900, 901]                     # vendor lines off the role axis: they enter the market only
SPLITS = {22: [(dt.date(2016, 6, 1), 2), (dt.date(2019, 2, 1), 3)], 900: [(dt.date(2015, 3, 2), 4)]}
LISTED = {55: dt.date(2016, 3, 1)}         # no vendor row before
ABOVE_CEILING = (66, dt.date(2018, 10, 1))  # an A9 row: line 66 withheld from here on
DUP = (33, dt.date(2017, 5, 3))            # a positive duplicate key: quarantined
GAP_DAYS = {11: 0.02}                      # line 11 has no row on 2% of sessions
PRICE_FIELDS = ["ret_overnight", "ret_intraday", "ceq_iss_5y", "coskew_60m", "vol_126"]
CUT = 150                                  # the mutation session (role row)


def day(d):
    return (d - EPOCH).days


def sessions(first, last):
    return [EPOCH + dt.timedelta(days=int(x)) for x in sec.nyse_sessions(first, last)]


def world(seed=20260929, ids=ROLE_IDS + OTHER_IDS, splits=SPLITS, listed=LISTED, breaks=()):
    """Vendor rows {column: list}. A common factor drives every line; dividends step F; splits step raw, F, shares;
    shares grow through issuance; ``breaks`` = [(date, k)]: every line's F re-anchored by k with no raw move."""
    rng = np.random.default_rng(seed)
    cal = sessions(TH_FIRST, TH_LAST)
    common = rng.normal(0.0003, 0.01, len(cal))
    rows = {k: [] for k in ("tradingDate", "securityID", "close", "open", "volume", "shares", "cumulReturnFactor")}
    for n, sid in enumerate(ids):
        beta = 0.6 + 0.2 * n
        lp, f, k_split = math.log(20.0 + 5 * n), 1.0, 1.0
        shares = 50_000 + 7_000 * n                        # thousands
        grow = (0.0, 0.10, -0.03, 0.20, 0.0, 0.05, 0.0, 0.0)[n % 8]
        miss = GAP_DAYS.get(sid, 0.0)
        for i, d in enumerate(cal):
            step = beta * common[i] + rng.normal(0.0, 0.015)
            lp += step
            if d.month in (3, 6, 9, 12) and d.day <= 3 and n % 3 == 2 and i and cal[i - 1].month != d.month:
                f *= 1.01                                  # a 1% dividend: F steps up, the raw close drops
                lp += math.log(1 / 1.01)
            for when, k in splits.get(sid, []):
                if d == when:
                    k_split *= k
                    f *= k
                    shares *= k
            for when, k in breaks:
                if d == when:
                    f *= k                                 # a vendor re-anchoring: F moves, the raw close does not
            if i and cal[i - 1].year != d.year:
                shares = int(round(shares * (1 + grow)))
            if sid in listed and d < listed[sid]:
                continue
            if miss and rng.random() < miss:
                continue
            raw = math.exp(lp) / k_split
            night = rng.normal(0.0, 0.004)
            for key, v in (("tradingDate", d), ("securityID", sid), ("close", raw), ("open", raw * math.exp(-night)),
                           ("volume", float(rng.integers(10_000, 90_000))), ("shares", int(shares)),
                           ("cumulReturnFactor", f)):
                rows[key].append(v)
    extra = [(DUP[1], DUP[0], 99.0), (dt.date(2017, 5, 6), 11, 50.0),        # duplicate key; a Saturday row
             (dt.date(2025, 2, 3), 22, 40.0)]                                  # a row after the seal
    for d, sid, v in extra:
        for key, x in (("tradingDate", d), ("securityID", sid), ("close", v), ("open", v), ("volume", 1e4),
                       ("shares", 1000), ("cumulReturnFactor", 1.0)):
            rows[key].append(x)
    sid, when = ABOVE_CEILING
    for i, (d, s) in enumerate(zip(rows["tradingDate"], rows["securityID"])):
        if d == when and s == sid:
            rows["shares"][i] = 150_000_000
    return rows


def write_source(path, rows, with_open=True):
    order = np.random.default_rng(7).permutation(len(rows["securityID"]))     # dates mixed in every row group
    cols = {"tradingDate": pa.date32(), "securityID": pa.int64(), "close": pa.float32(), "volume": pa.float64(),
            "shares": pa.int64(), "cumulReturnFactor": pa.float64()}
    if with_open:
        cols["open"] = pa.float32()
    pq.write_table(pa.table({k: pa.array([rows[k][i] for i in order], t) for k, t in cols.items()}), path,
                   row_group_size=1500)
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def observations(rows):
    """{(sid, epoch day): (raw f32 as f64, F, shares, open f32 as f64, volume)} of the unique, valid, on-calendar
    rows (the observation contract), duplicates dropped."""
    seen, keep = {}, {}
    for i, (d, s) in enumerate(zip(rows["tradingDate"], rows["securityID"])):
        seen[(s, day(d))] = seen.get((s, day(d)), 0) + 1
        keep[(s, day(d))] = i
    cal = set(int(x) for x in sec.nyse_sessions(TH_FIRST, TH_LAST))
    out = {}
    for key, i in keep.items():
        raw, f, v = float(np.float32(rows["close"][i])), rows["cumulReturnFactor"][i], rows["volume"][i]
        if seen[key] != 1 or key[1] not in cal or key[1] >= day(dt.date(2025, 1, 1)):
            continue
        if raw > 0 and f > 0 and v >= 0:
            out[key] = (raw, f, rows["shares"][i], float(np.float32(rows["open"][i])), v)
    return out


def write_role(root, rows, source_sha, ids=ROLE_IDS):
    """The role projected from the vendor rows (close = raw x F, present = an observation)."""
    root.mkdir(parents=True)
    obs = observations(rows)
    days = [day(d) for d in sessions(ROLE_FIRST, ROLE_LAST)]
    nd, n = len(days), len(ids)
    close, raw, volume = (np.full((nd, n), np.nan) for _ in range(3))
    present = np.zeros((nd, n), dtype="u1")
    for t, d in enumerate(days):
        for j, sid in enumerate(ids):
            o = obs.get((sid, d))
            if o:
                raw[t, j], close[t, j], volume[t, j], present[t, j] = o[0], o[0] * o[1], o[4], 1
    member = present.copy()
    member[5:9, 0] = 0
    blobs = {"sessions.i64": np.array([d * DAY_NS for d in days], dtype="<i8").tobytes(),
             "ids.u64": np.array(ids, dtype="<u8").tobytes(), "member.u8": member.tobytes(),
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


class Case:
    """One world on disk: vendor source, role, and builder runs."""

    def __init__(self, base: Path, rows, name="w", with_open=True, ids=ROLE_IDS):
        self.base, self.rows, self.ids = base, rows, ids
        self.src = base / f"{name}.parquet"
        sha = write_source(self.src, rows, with_open)
        self.role = base / f"{name}-role"
        self.role_sha = write_role(self.role, rows, sha, ids)
        self.days = [day(d) for d in sessions(ROLE_FIRST, ROLE_LAST)]

    def run(self, out, fields, **kw):
        kw.setdefault("module_options", {"price_source": self.src})
        with contextlib.redirect_stdout(io.StringIO()):
            return tool.run(self.role, self.role_sha, self.base / out, fields, **kw)

    def field(self, out, name):
        return np.fromfile(self.base / out / f"{name}.f64", dtype="<f8").reshape(len(self.days), len(self.ids))


def calendar():
    """The module's extended axis for this world (NYSE sessions before the role; the role's sessions are NYSE
    sessions too) as epoch days, reaching back further than any row."""
    return [int(x) for x in sec.nyse_sessions(TH_FIRST - dt.timedelta(days=800), ROLE_LAST)]


# ---- independent definitions -------------------------------------------------------------------------------------

def ref_open(obs, cal, t_day_index, sid):
    a, b = cal[t_day_index - 1], cal[t_day_index - 2]
    oa, ob = obs.get((sid, a)), obs.get((sid, b))
    night = intra = math.nan
    if oa:
        c, f, _, o, _ = oa
        if abs(math.log(c / o)) <= 1.5:
            intra = c / o - 1
        if ob:
            cb, fb = ob[0], ob[1]
            adj, rw = math.log(o * f) - math.log(cb * fb), math.log(o) - math.log(cb)
            if abs(adj) <= 1.5 and abs(adj) <= abs(rw) + 0.10:
                night = o * f / (cb * fb) - 1
    return night, intra


def ref_ceq(obs, cal, t_idx, sid, first_above):
    a, b = cal[t_idx - 1], cal[t_idx - 1 - 1260]

    def q(s):
        cands = [d for (x, d) in obs if x == sid and s - 490 <= d <= s - 90 and 0 < obs[(x, d)][2] <= 1e8]
        if not cands:
            return None
        o = obs[(sid, max(cands))]
        return o[2] / o[1]
    if (sid, a) not in obs or (sid, b) not in obs or first_above.get(sid, 10 ** 9) <= a:
        return math.nan
    qa, qb = q(a), q(b)
    if qa is None or qb is None:
        return math.nan
    v = math.log(qa) - math.log(qb)
    return v if abs(v) <= math.log(100.0) else math.nan


def ref_market(obs, cal):
    """Daily vendor EW market: {epoch day: mu}."""
    ids = sorted({s for s, _ in obs})
    mu = {}
    for i in range(1, len(cal)):
        rs = []
        for sid in ids:
            p, c = obs.get((sid, cal[i - 1])), obs.get((sid, cal[i]))
            if not (p and c):
                continue
            r, s = math.log(c[0]) - math.log(p[0]), math.log(c[1]) - math.log(p[1])
            a = r + s
            if abs(a) > 1.5 or abs(a) > abs(r) + 0.10 or (abs(s) > 0.01 and abs(a) > abs(r) + 0.01):
                continue
            rs.append(math.exp(a) - 1)
        mu[cal[i]] = math.fsum(rs) / len(rs) if rs else math.nan
    return mu


def ref_coskew(obs, cal, mu, t_idx, sid):
    ends = [t_idx - 1 - 21 * k for k in range(61)]
    r, m = [], []
    for k in range(60):
        e, s = cal[ends[k]], cal[ends[k + 1]]
        days_in = cal[ends[k + 1] + 1: ends[k] + 1]
        mk = float(np.prod([1 + mu.get(d, math.nan) for d in days_in])) - 1
        pe, ps = obs.get((sid, e)), obs.get((sid, s))
        if not (pe and ps) or not math.isfinite(mk):
            continue
        la = math.log(pe[0] * pe[1]) - math.log(ps[0] * ps[1])
        lr = math.log(pe[0]) - math.log(ps[0])
        if abs(la) > abs(lr) + 0.10:
            continue
        r.append(pe[0] * pe[1] / (ps[0] * ps[1]) - 1)
        m.append(mk)
    if len(r) < 48:
        return math.nan
    r, m = np.array(r), np.array(m)
    x = np.column_stack((np.ones(len(m)), m))
    coef = np.linalg.lstsq(x, r, rcond=None)[0]
    eps = r - x @ coef
    em = m - m.mean()
    return float(np.mean(eps * em ** 2) / (math.sqrt(np.mean(eps ** 2)) * np.mean(em ** 2)))


class PriceFields(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_field_at_t_unchanged_when_rows_after_t_mutate(self):
        rows = world()
        base = Case(self.base, rows, "base")
        base.run("f0", PRICE_FIELDS)
        cut_day = base.days[CUT]
        rng = np.random.default_rng(5)
        late = dict((k, list(v)) for k, v in rows.items())
        for i, d in enumerate(late["tradingDate"]):
            if day(d) >= cut_day:     # every vendor row dated on or after session CUT, off-axis lines included
                late["close"][i] *= float(rng.uniform(0.5, 1.5))
                late["open"][i] *= float(rng.uniform(0.5, 1.5))
                late["cumulReturnFactor"][i] *= float(rng.uniform(0.9, 1.1))
                late["shares"][i] = int(late["shares"][i] * rng.uniform(0.5, 2.0))
                late["volume"][i] *= float(rng.uniform(0.1, 10.0))
        mutated = Case(self.base, late, "late")
        mutated.run("f1", PRICE_FIELDS)
        for name in PRICE_FIELDS:
            a, b = base.field("f0", name), mutated.field("f1", name)
            self.assertEqual(a[:CUT + 1].tobytes(), b[:CUT + 1].tobytes(), name)   # rows <= CUT: bit for bit
            if name != "ceq_iss_5y":   # (shares enter ceq through the 90-day lag: later rows need not move yet)
                self.assertNotEqual(a[CUT + 2:].tobytes(), b[CUT + 2:].tobytes(), name)
        # xrd0_ttm: its inputs are this run's lagged fundamentals rows; a later row never moves row t
        role = tool.Role(base.role, base.role_sha)
        host = tool.FIELD_MODULES[-1].h
        outs = []
        for k, out in enumerate(("x0", "x1")):
            d = self.base / out
            d.mkdir()
            g = np.random.default_rng(3)
            xrd = np.where(g.random((role.n_dates, role.n)) < 0.3, np.nan, g.uniform(1e6, 1e8, (role.n_dates, role.n)))
            sale = np.where(g.random((role.n_dates, role.n)) < 0.2, np.nan, g.uniform(1e7, 1e9, (role.n_dates, role.n)))
            if k:
                xrd[CUT + 1:] = np.where(np.isnan(xrd[CUT + 1:]), 5.0, np.nan)
                sale[CUT + 1:] = 7.0
            xrd.astype("<f8").tofile(d / "xrd_ttm.f64")
            sale.astype("<f8").tofile(d / "sale_ttm.f64")
            price.zero_filled_rows(host, role, d, tool.Budget(700, 600))
            outs.append(np.fromfile(d / "xrd0_ttm.f64", dtype="<f8").reshape(role.n_dates, role.n))
            np.testing.assert_array_equal(outs[-1], np.where(np.isfinite(xrd), xrd,
                                                             np.where(np.isfinite(sale), 0.0, np.nan)))
        self.assertEqual(outs[0][:CUT + 1].tobytes(), outs[1][:CUT + 1].tobytes())
        self.assertNotEqual(outs[0][CUT + 1:].tobytes(), outs[1][CUT + 1:].tobytes())

    def test_ceq_iss_split_invariant(self):
        """Two lines with the same economics; the second splits 2:1, 3:1 and 1:2 (reverse). Every row equal."""
        twin_splits = {2: [(dt.date(2014, 2, 3), 2), (dt.date(2016, 6, 1), 3), (dt.date(2019, 2, 1), 0.5)]}
        rows = world(seed=11, ids=[1, 2], splits=twin_splits, listed={})
        # line 2 copies line 1's path: rebuild its rows from line 1's with the split terms applied
        one = [i for i, s in enumerate(rows["securityID"]) if s == 1]
        two = [i for i, s in enumerate(rows["securityID"]) if s == 2]
        self.assertEqual([rows["tradingDate"][i] for i in one], [rows["tradingDate"][i] for i in two])
        for i1, i2 in zip(one, two):
            k = 1.0
            for when, x in twin_splits[2]:
                if rows["tradingDate"][i1] >= when:
                    k *= x
            rows["close"][i2] = rows["close"][i1] / k
            rows["open"][i2] = rows["open"][i1] / k
            rows["cumulReturnFactor"][i2] = rows["cumulReturnFactor"][i1] * k
            rows["shares"][i2] = int(round(rows["shares"][i1] * k))
            rows["volume"][i2] = rows["volume"][i1]
        case = Case(self.base, rows, "split", ids=[1, 2])
        case.run("f", ["ceq_iss_5y"])
        v = case.field("f", "ceq_iss_5y")
        self.assertTrue(np.isfinite(v).all())
        np.testing.assert_allclose(v[:, 1], v[:, 0], rtol=0, atol=1e-12)
        # and the value is the definition: ln of the growth of shares / F between the two lagged observations
        obs = observations(rows)
        cal = calendar()
        for t in (0, 60, len(case.days) - 1):
            ti = cal.index(case.days[t])
            self.assertAlmostEqual(v[t, 0], ref_ceq(obs, cal, ti, 1, {}), places=12)

    def test_factor_break_step_is_chained(self):
        """A vendor re-anchoring on one session for every line (a factor-break-v1 mass session): repaired, so
        ceq_iss_5y and the overnight return equal the world without it."""
        ids = list(range(101, 153))            # 52 lines >= the 50-cell mass threshold
        when = dt.date(2016, 8, 16)     # mid-month: no dividend step on the same session
        plain = Case(self.base, world(seed=3, ids=ids, splits={}, listed={}), "plain", ids=ids)
        broken = Case(self.base, world(seed=3, ids=ids, splits={}, listed={}, breaks=[(when, 1.3)]), "broken",
                      ids=ids)
        plain.run("p", ["ceq_iss_5y"])
        m = broken.run("b", ["ceq_iss_5y"])
        a, b = plain.field("p", "ceq_iss_5y"), broken.field("b", "ceq_iss_5y")
        self.assertTrue(np.isfinite(a).any())
        np.testing.assert_allclose(b, a, rtol=0, atol=1e-12)
        fb = m["source_checks"]["price"]["source"]["factor_break"]
        self.assertEqual((fb["mass_sessions"], fb["repaired_steps"]), ([when.isoformat()], len(ids)))

    def test_coskew_matches_numpy_reference(self):
        rows = world()
        case = Case(self.base, rows)
        m = case.run("f", ["coskew_60m"])
        got = case.field("f", "coskew_60m")
        obs = observations(rows)
        cal = calendar()
        mu = ref_market(obs, [d for d in cal if d >= day(TH_FIRST)])
        want = np.full_like(got, np.nan)
        for t in range(0, len(case.days), 7):
            ti = cal.index(case.days[t])
            for j, sid in enumerate(ROLE_IDS):
                want[t, j] = ref_coskew(obs, cal, mu, ti, sid)
        rows_checked = list(range(0, len(case.days), 7))
        np.testing.assert_allclose(got[rows_checked], want[rows_checked], rtol=1e-9, atol=1e-12)
        self.assertTrue(np.isfinite(got[rows_checked]).sum() >= 4 * len(rows_checked))
        self.assertTrue(np.isnan(got[:, ROLE_IDS.index(55)]).all())   # listed 2016-03: fewer than 48 months
        st = m["source_checks"]["price"]["market"]
        self.assertEqual(st["contributors_max"], len(ROLE_IDS) + len(OTHER_IDS))

    def test_values_match_definitions(self):
        rows = world()
        case = Case(self.base, rows)
        m = case.run("f", PRICE_FIELDS)
        obs = observations(rows)
        cal = calendar()
        first_above = {ABOVE_CEILING[0]: day(ABOVE_CEILING[1])}
        for t in range(0, len(case.days), 9):
            ti = cal.index(case.days[t])
            for j, sid in enumerate(ROLE_IDS):
                night, intra = ref_open(obs, cal, ti, sid)
                for name, want in (("ret_overnight", night), ("ret_intraday", intra),
                                   ("ceq_iss_5y", ref_ceq(obs, cal, ti, sid, first_above))):
                    got = case.field("f", name)[t, j]
                    if math.isnan(want):
                        self.assertTrue(math.isnan(got), (name, t, sid))
                    else:
                        self.assertAlmostEqual(got, want, places=12, msg=(name, t, sid))
        vol = case.field("f", "vol_126")
        role_obs = [[obs.get((sid, d)) for sid in ROLE_IDS] for d in case.days]
        for t in (0, 125, 126, 150, len(case.days) - 1):
            for j in range(len(ROLE_IDS)):
                vs = [role_obs[s][j][4] for s in range(max(t - 126, 0), t) if role_obs[s][j]]
                want = sum(vs) / len(vs) if t >= 126 and len(vs) >= 63 else math.nan
                if math.isnan(want):
                    self.assertTrue(math.isnan(vol[t, j]))
                else:
                    self.assertAlmostEqual(vol[t, j], want, places=6)
        ceq = case.field("f", "ceq_iss_5y")
        c66 = ROLE_IDS.index(66)
        withheld = [t for t, d in enumerate(case.days) if case.days[max(t - 1, 0)] >= day(ABOVE_CEILING[1])]
        self.assertTrue(withheld and np.isnan(ceq[withheld, c66]).all())
        self.assertTrue(np.isfinite(ceq[:withheld[0], c66]).any())
        e = {x["name"]: x for x in m["fields"]}
        for name in PRICE_FIELDS:
            self.assertTrue(e[name]["point_in_time"])
            self.assertEqual(e[name]["producer"]["module"], "research_fields_price.py")
            self.assertEqual(e[name]["producer"]["code_sha256_lf"], tool.module_code_identity(price)["code_sha256_lf"])
            self.assertEqual(e[name]["formula_sha256"], tool.formula_id(name, tool.spec_definition(name, 1)))
        self.assertEqual(m["source_checks"]["price"]["source"]["duplicate_keys_quarantined"], 1)
        self.assertEqual(m["source_checks"]["price"]["source"]["rows_off_calendar"], 1)
        self.assertEqual(m["source_checks"]["price"]["source"]["rows_on_or_after_seal_skipped"], 1)

    def test_open_capability_and_refusals(self):
        rows = world()
        case = Case(self.base, rows, "noopen", with_open=False)
        for name in price.OPEN_NAMES:
            with self.assertRaisesRegex(price.FieldNeedsOpen, "field needs open; absent in export"):
                case.run("o-" + name, [name])
            self.assertFalse((self.base / ("o-" + name)).exists())
        case.run("ok", ["ceq_iss_5y", "vol_126"])        # the other fields do not need the open
        with self.assertRaisesRegex(ValueError, "need --price-source"):
            case.run("x1", ["coskew_60m"], module_options={})
        self.assertFalse((self.base / "x1").exists())
        other = Case(self.base, world(seed=9), "other")
        with self.assertRaisesRegex(ValueError, "differs from the role's source_sha256"):
            case.run("x2", ["ceq_iss_5y"], module_options={"price_source": other.src})
        with self.assertRaisesRegex(ValueError, "requires xrd_ttm, sale_ttm"):
            case.run("x3", ["xrd0_ttm"])
        # the CLI: --price-source reaches the module
        good = Case(self.base, rows, "cli")
        argv = ["--role", str(good.role), "--role-sha256", good.role_sha, "--output", str(self.base / "cli-out"),
                "--fields", "ret_intraday,vol_126", "--price-source", str(good.src)]
        with contextlib.redirect_stdout(io.StringIO()):
            tool.main(argv)
        good.run("api", ["ret_intraday", "vol_126"])
        for name in ("ret_intraday", "vol_126"):
            self.assertEqual((self.base / "cli-out" / f"{name}.f64").read_bytes(),
                             (self.base / "api" / f"{name}.f64").read_bytes())

    def test_reuse_with_price_fields(self):
        """Ruling E-21: a --reuse build that includes F-1 fields runs through the C-3 interface (it used to raise
        AttributeError after creating the output), copies unchanged payloads byte for byte and recomputes only what
        the prior lacks or what its producing code changed; a module without the interface is refused before any
        output exists."""
        case = Case(self.base, world())
        full = case.run("full", PRICE_FIELDS)
        case.run("part", ["ceq_iss_5y", "vol_126"])
        mixed = case.run("mixed", PRICE_FIELDS, reuse=self.base / "part")
        block = mixed["reuse"]
        self.assertEqual((block["reused"], block["computed"]),
                         (["ceq_iss_5y", "vol_126"], ["ret_overnight", "ret_intraday", "coskew_60m"]))
        self.assertEqual(block["not_reused"]["coskew_60m"], "absent from the prior manifest")
        self.assertEqual(mixed["files"], full["files"])        # reused and recomputed payloads equal a fresh build
        again = case.run("again", PRICE_FIELDS, reuse=self.base / "mixed")
        self.assertEqual((again["reuse"]["reused"], again["reuse"]["computed"], again["reuse"]["not_reused"]),
                         (PRICE_FIELDS, [], {}))
        for name in PRICE_FIELDS:
            self.assertEqual((self.base / "again" / f"{name}.f64").read_bytes(),
                             (self.base / "full" / f"{name}.f64").read_bytes(), name)
            e = dict(next(x for x in again["fields"] if x["name"] == name))
            rec = e.pop("reused_from")
            self.assertEqual(e, next(x for x in full["fields"] if x["name"] == name), name)   # the entry verbatim
            self.assertEqual(rec["producer"]["module"], "research_fields_price.py", name)
            self.assertEqual(rec["inputs"], {}, name)
        chained = case.run("chained", PRICE_FIELDS, reuse=self.base / "again")   # the origin's producer is kept
        self.assertEqual(chained["reuse"]["reused"], PRICE_FIELDS)
        # one producer edit (coskew_rows) recomputes only its group; the module blob is in git's object store
        path = Path(price.__file__)
        original = path.read_bytes().replace(b"\r\n", b"\n")
        head = b"def coskew_rows(h, panel: dict, mu: np.ndarray, role, output: Path, budget) -> dict:"
        self.assertEqual(original.count(head), 1)
        edited = original.replace(head, head[:-len(b") -> dict:")] + b", _edited=None) -> dict:")
        blobs = {tool.code_identity_of(original)["code_git_blob_sha1"]: original}
        real_source, real_blob = tool.module_source, tool.git_blob
        with unittest.mock.patch.object(tool, "module_source", lambda m: edited if m is price else real_source(m)), \
                unittest.mock.patch.object(tool, "git_blob", lambda sha1: blobs.get(sha1) or real_blob(sha1)):
            one = case.run("one", PRICE_FIELDS, reuse=self.base / "again")
        self.assertEqual(one["reuse"]["computed"], ["coskew_60m"])
        self.assertIn("group price_coskew", one["reuse"]["not_reused"]["coskew_60m"])
        self.assertEqual(one["files"], full["files"])
        # a prior written before E-21 (entries with producer_code, no producer): recomputed, never guessed
        legacy = self.base / "legacy"
        legacy.mkdir()
        for name in ("ceq_iss_5y.f64", "vol_126.f64"):
            (legacy / name).write_bytes((self.base / "part" / name).read_bytes())
        m = json.loads((self.base / "part" / "manifest.json").read_text(encoding="utf-8"))
        for e in m["fields"]:
            e["producer_code"] = e.pop("producer")
        (legacy / "manifest.json").write_text(json.dumps(m), encoding="utf-8")
        old = case.run("old", ["ceq_iss_5y", "vol_126"], reuse=legacy)
        self.assertEqual(old["reuse"]["computed"], ["ceq_iss_5y", "vol_126"])
        self.assertIn("producing code not recoverable", old["reuse"]["not_reused"]["vol_126"])
        # the interface is checked before the output directory is created
        saved = price.HOST_HANDLES
        del price.HOST_HANDLES
        try:
            with self.assertRaisesRegex(ValueError, r"research_fields_price.py lacks the reuse interface "
                                                    r"\(HOST_HANDLES\)"):
                case.run("refused", ["vol_126"], reuse=self.base / "full")
        finally:
            price.HOST_HANDLES = saved
        self.assertFalse((self.base / "refused").exists())
        for name in price.FIELDS:
            self.assertEqual(price.producer_group(name), price.FIELDS[name]["group"])
            self.assertIs(price.field_spec(name), price.FIELDS[name])

    def test_producers_cover_every_field(self):
        self.assertEqual(sorted({s["group"] for s in price.FIELDS.values()}), sorted(price.PRODUCERS))
        for group, entries in price.PRODUCERS.items():
            for fn in entries:
                self.assertTrue(callable(getattr(price, fn)), (group, fn))
        names = list(tool.ALL_FIELDS)   # one block after every builder and SEC field (later modules follow it)
        start = names.index(next(iter(price.FIELDS)))
        self.assertEqual(names[start:start + len(price.FIELDS)], list(price.FIELDS))
        self.assertTrue(set(names[:start]) >= set(tool.FIELDS) | set(tool.ISSUER_FIELDS) | set(sec.FIELDS))
        self.assertTrue(all(tool.ALL_FIELDS[x]["point_in_time"] for x in price.FIELDS))
        self.assertFalse(set(price.FIELDS) & set(tool.DEFAULT_FIELDS))   # opt-in: the default build is unchanged


if __name__ == "__main__":
    unittest.main()
