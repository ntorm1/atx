"""research_fields_v8 fiscal-quarter fields (platform v8 F-C gscore7_lowbm; Ruling E-20).

Synthetic world dated 2014-2021 only: a TickerHistory3-like file (shares_out, hence me_company), a role of the NYSE
sessions 2021-03-01 .. 2021-06-30 (15 lines: 13 primary issuers, a second class of the first, an unlinked line), a
pinned identity bridge and an atx.fundamental-events/v1 artifact with quarterly rows 2014-2021 per issuer (an
amendment, a missing quarter, an annual-only filer, a short history, a null period_end, a split, a |g_0| > 6 jump, a
sign flip, non-positive shares, missing R&D and capex, a negative book equity). Every value is checked against an
independent oracle written from the definitions (QUARTER_RULE, GSCORE_RULE)."""
import contextlib
import datetime as dt
import hashlib
import io
import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import prepare_research_fields as tool
import research_fields_sec as sec
import research_fields_v8 as v8
import test_prepare_research_fields as base

EPOCH = dt.date(1970, 1, 1)
UTC = dt.timezone.utc
SESSIONS = [EPOCH + dt.timedelta(days=int(x)) for x in sec.nyse_sessions(dt.date(2021, 3, 1), dt.date(2021, 6, 30))]
N_ISSUERS, N_LINES = 13, 15
SIDS = [11 + j for j in range(N_LINES)]
LAG, CUT = 1, 40
BM_TARGET = [0.2, 0.3, 0.25, 0.4, 1.0, 1.2, 0.15, 0.35, 2.0, 0.22, -0.1, 0.28, 0.5]
SICS = (3571, 7372, 2834)
NEW = ["gscore7_lowbm"]
BASE_FIELDS = ["si_shares", "shares_out", "me_company", "grp_sic2"]    # shares_out's units rule reads si_shares
FINRA_ROWS = [(11, "2021-02-01", "100"), (12, "2021-03-15", "50")]
ITEMS = ("be", "at", "ni_ttm", "cfo_ttm", "capx_ttm", "xrd_ttm", "ni_q", "sale_ttm", "shrs_q")


def day(d):
    return (d - EPOCH).days


def mark(d):
    return dt.datetime.combine(d, dt.time(22), tzinfo=UTC)


def quarter_ends(first=dt.date(2014, 3, 31), last=dt.date(2021, 3, 31)):
    out, (y, q) = [], (first.year, (first.month - 1) // 3 + 1)
    while True:
        m = 3 * q
        p = dt.date(y + m // 12, m % 12 + 1, 1) - dt.timedelta(days=1)
        if p > last:
            return out
        out.append(p)
        y, q = (y + 1, 1) if q == 4 else (y, q + 1)


# ---- the world -----------------------------------------------------------------------------------------------------
def th_days():
    return [EPOCH + dt.timedelta(days=int(x)) for x in sec.nyse_sessions(dt.date(2019, 9, 2), SESSIONS[-1])]


def close_of(j, i):
    return float(np.float32((10 + j) * (1 + 0.0005 * i)))


def write_th(path):
    days = th_days()
    rows = {k: [] for k in ("tradingDate", "securityID", "close", "volume", "shares", "cumulReturnFactor")}
    for j, sid in enumerate(SIDS):
        for i, d in enumerate(days):
            for k, v in (("tradingDate", d), ("securityID", sid), ("close", close_of(j, i)), ("volume", 1e5),
                         ("shares", 1000 + 100 * j), ("cumulReturnFactor", 1.0)):
                rows[k].append(v)
    types = {"tradingDate": pa.date32(), "securityID": pa.int64(), "close": pa.float32(), "volume": pa.float64(),
             "shares": pa.int64(), "cumulReturnFactor": pa.float64()}
    pq.write_table(pa.table({k: pa.array(rows[k], t) for k, t in types.items()}), path, row_group_size=2000)
    return base.sha(path)


def write_role(root, th_sha, raw_scale=None):
    """The role projected from the vendor rows; ``raw_scale`` multiplies raw_close from row CUT on (a mutation)."""
    root.mkdir(parents=True)
    index = {d: i for i, d in enumerate(th_days())}
    raw = np.array([[close_of(j, index[d]) for j in range(N_LINES)] for d in SESSIONS])
    if raw_scale is not None:
        raw[CUT:] *= raw_scale
    member = np.ones(raw.shape, dtype="u1")
    member[0:5, 2] = 0
    member[20:30, 6] = 0
    blobs = {"sessions.i64": np.array([day(d) * base.DAY_NS for d in SESSIONS], dtype="<i8").tobytes(),
             "ids.u64": np.array(SIDS, dtype="<u8").tobytes(), "member.u8": member.tobytes(),
             "close.f64": raw.astype("<f8").tobytes(), "raw_close.f64": raw.astype("<f8").tobytes(),
             "present.u8": np.ones(raw.shape, dtype="u1").tobytes(),
             "volume.f64": np.full(raw.shape, 1e5).astype("<f8").tobytes()}
    files = {}
    for name, blob in blobs.items():
        (root / name).write_bytes(blob)
        files[name] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete", "dates": len(SESSIONS),
                "instruments": N_LINES, "instrument_namespace": "spiderrock.securityID", "score_begin": 5,
                "score_end": len(SESSIONS), "source_sha256": th_sha, "files": files,
                "clock_recipe": "modeled-session+22h-mark+23h-decision-v1"}
    (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return base.sha(root / "manifest.json")


BRIDGE = ([(SIDS[j], 5000 + j, dt.date(2015, 1, 1), None, base.LONG_AGO, "P", "high", "reconstructed_high")
           for j in range(N_ISSUERS)]
          + [(SIDS[13], 5000, dt.date(2015, 1, 1), None, base.LONG_AGO, "J", "high", "reconstructed_high")])
SIC_EVENTS = ([(5000 + j, base.at(2020, 5, 1, 12), SICS[j % 3], f"{5000 + j:010d}-20-900000") for j in range(N_ISSUERS)]
              + [(5012, base.at(2021, 4, 15, 12), 7373, "0000005012-21-900001")])


def events_rows():
    """Quarterly events rows of the 13 issuers (dicts; accepted_utc tz-aware)."""
    rng = np.random.default_rng(20260930)
    rows = []
    for j in range(N_ISSUERS):
        cik, ends = 5000 + j, quarter_ends()
        if j == 5:
            ends = [p for p in ends if p.month == 12]                      # annual-only filer (400 days)
        if j == 12:
            ends = [p for p in ends if p >= dt.date(2018, 3, 31)]          # short history
        if j == 3:
            ends = [p for p in ends if p != dt.date(2019, 6, 30)]          # a missing quarter
        me = (1000 + 100 * j) * 1000.0 * (10 + j)
        for k, p in enumerate(ends):
            kq = (p.year - 2014) * 4 + (p.month - 1) // 3
            shrs = 1e6 * (1 + j / 10) * (2.0 if j == 1 and p >= dt.date(2018, 12, 31) else 1.0)   # j = 1: a split
            eps = (0.5 + 0.05 * j) * (1 + 0.03 * kq) + float(rng.normal(0, 0.01))
            if j == 6 and p == dt.date(2020, 12, 31):
                eps *= 20.0                                                # |g_0| > 6
            if j == 7 and p.month == 12 and p.year in (2019, 2020):
                eps = (2.0 if p.year == 2019 else 0.2) * (0.85 + 0.01 * kq)   # g_4 > 0 then g_0 < 0
            at = 1e8 * (1 + 0.1 * j) * (1 + 0.01 * kq)
            item = {"shrs_q": 0.0 if j == 8 and p.year == 2019 else shrs, "ni_q": eps * shrs, "at": at,
                    "be": me * BM_TARGET[j] * (1 + 0.005 * kq), "ni_ttm": 4 * eps * shrs * (1 + float(rng.normal(0, .1))),
                    "sale_ttm": 1e7 * (1 + 0.1 * j) * (1 + 0.03 * kq) * (1 + float(rng.normal(0, 0.02))),
                    "capx_ttm": math.nan if j == 11 else at * 0.02 * (1 + j % 3) * (1 + float(rng.normal(0, .1))),
                    "xrd_ttm": math.nan if j == 9 else at * 0.01 * (j % 4) * (1 + float(rng.normal(0, .1)))}
            item["cfo_ttm"] = item["ni_ttm"] * (0.7 + 0.1 * (j % 4)) + float(rng.normal(0, 1e5))
            filed = p + dt.timedelta(days=40 + j % 5)
            rows.append({"cik": cik, "accepted_utc": dt.datetime.combine(filed, dt.time(20, 30 if j == 2 else 0),
                                                                         tzinfo=UTC),
                         "accession": f"{cik:010d}-{p.year % 100:02d}-{k:06d}", "period_end": p,
                         "staleness_days": 400 if j == 5 else 200, **item})
            if j == 4 and p == dt.date(2020, 9, 30):                       # an amendment: the same anchor, later
                rows.append({**rows[-1], "accepted_utc": rows[-1]["accepted_utc"] + dt.timedelta(days=10),
                             "accession": f"{cik:010d}-20-{900 + k:06d}", "ni_q": item["ni_q"] * 1.5,
                             "sale_ttm": item["sale_ttm"] * 1.1})
    rows.append({**next(r for r in rows if r["cik"] == 5002 and r["period_end"] == dt.date(2019, 12, 31)),
                 "accepted_utc": base.at(2020, 6, 1, 12), "accession": "0000005002-20-000999",
                 "period_end": None})                                     # a null period_end: never a quarter
    return rows


def write_events(root, rows, sic_events=SIC_EVENTS):
    data = {"cik": pa.array([r["cik"] for r in rows], pa.int64()),
            "accession": pa.array([r["accession"] for r in rows], pa.string()),
            "accepted_utc": pa.array([r["accepted_utc"] for r in rows], pa.timestamp("us", tz="UTC")),
            "clock_basis": pa.array(["fsds_accepted_utc"] * len(rows), pa.string()),
            "period_end": pa.array([r["period_end"] for r in rows], pa.date32()),
            "staleness_days": pa.array([r["staleness_days"] for r in rows], pa.int32()),
            **{k: pa.array([r[k] for r in rows], pa.float64()) for k in ITEMS}}
    s = list(zip(*sic_events))
    sic = pa.table({"cik": pa.array(s[0], pa.int64()), "accession": pa.array(s[3], pa.string()),
                    "accepted_utc": pa.array(s[1], pa.timestamp("us", tz="UTC")),
                    "clock_basis": pa.array(["fsds_accepted_utc"] * len(s[0]), pa.string()),
                    "sic": pa.array(s[2], pa.int32())})
    return base.write_published(root, "atx.fundamental-events/v1",
                                {"fundamental_events.parquet": pa.table(data), "sic_events.parquet": sic},
                                values_label="modeled_unaccepted", rehearsal_identity=True, items=list(ITEMS))


class World:
    def __init__(self, root: Path, rows=None, sic_events=SIC_EVENTS, raw_scale=None, name="w"):
        self.base = root
        self.rows = events_rows() if rows is None else rows
        self.th = root / "TickerHistory3.parquet"
        th_sha = write_th(self.th) if not self.th.exists() else base.sha(self.th)
        self.role = root / f"{name}-role"
        self.role_sha = write_role(self.role, th_sha, raw_scale)
        self.bridge = root / "bridge"
        self.bridge_sha = base.write_bridge(self.bridge, rows=BRIDGE) if not self.bridge.exists() else \
            base.sha(self.bridge / "manifest.json")
        self.finra = root / "finra"
        if not self.finra.exists():
            base.write_finra(self.finra, shares_rows=FINRA_ROWS, dtc_rows=FINRA_ROWS[:1])
        self.events = root / f"{name}-events"
        self.events_sha = write_events(self.events, self.rows, sic_events)

    def options(self, **over):
        return {"identity_bridge": self.bridge, "identity_bridge_sha256": self.bridge_sha,
                "fund_events": self.events, "fund_events_sha256": self.events_sha, "fund_lag_sessions": LAG, **over}

    def run(self, out, fields=BASE_FIELDS + NEW, **kw):
        lag = kw.pop("fund_lag_sessions", LAG)
        kw.setdefault("module_options", self.options(fund_lag_sessions=lag))
        with contextlib.redirect_stdout(io.StringIO()):
            return tool.run(self.role, self.role_sha, self.base / out, list(fields), tickerhistory=self.th, finra=self.finra,
                            identity_bridge=self.bridge, identity_bridge_sha256=self.bridge_sha,
                            fund_events=self.events, fund_events_sha256=self.events_sha, fund_lag_sessions=lag, **kw)

    def field(self, out, name):
        return np.fromfile(self.base / out / f"{name}.f64", dtype="<f8").reshape(len(SESSIONS), N_LINES)


# ---- the oracle ----------------------------------------------------------------------------------------------------
def quarter_view(view, quarters):
    """QUARTER_RULE: [row or None] for k = 0..quarters-1 from the rows of one CIK up to the selected (last) one."""
    a = view[-1]["period_end"]
    if a is None:
        return [None] * quarters
    anchors = sorted({r["period_end"] for r in view if r["period_end"] is not None})
    out = []
    for k in range(quarters):
        target = a - dt.timedelta(days=math.floor(91.3125 * k + 0.5))
        near = [p for p in anchors if abs((p - target).days) <= 20]
        if not near:
            out.append(None)
            continue
        p = max(near, key=lambda x: (-abs((x - target).days), x))
        out.append([r for r in view if r["period_end"] == p][-1])
    return out


def val(r, key):
    return math.nan if r is None else r[key]


def variance(xs):
    fin = [x for x in xs if math.isfinite(x)]
    return float(np.var(fin, ddof=1)) if len(fin) >= 12 else math.nan


def oracle_history(view):
    """(VARROA, VARSGR) of the selected row's view."""
    qv = quarter_view(view, 17)
    roa = [val(r, "ni_q") / val(r, "at") if val(r, "at") > 0 else math.nan for r in qv[:16]]
    sale = [val(r, "sale_ttm") for r in qv]
    sg = [sale[k] / sale[k + 1] - 1 if sale[k + 1] > 0 else math.nan for k in range(16)]
    return variance(roa), variance(sg)


def selected_view(rows, sid, t, seal):
    """(view of the selected events row, its row) of line sid at session t, or None (the issuer fields' rule)."""
    d = SESSIONS[t]
    links = [(cik, kind) for s, cik, start, end, avail, kind, _, _ in BRIDGE if s == sid]
    if not links or links[0][1] != "P" or t < LAG:
        return None
    cut = mark(SESSIONS[t - LAG])
    mine = sorted((r for r in rows if r["cik"] == links[0][0] and r["accepted_utc"] < seal),
                  key=lambda r: (r["accepted_utc"], r["accession"]))
    seen = [r for r in mine if r["accepted_utc"] < cut]
    if not seen:
        return None
    last = seen[-1]
    if last["period_end"] is None or (d - last["period_end"]).days > last["staleness_days"]:
        return None
    return seen


def oracle(rows, me, sic2, member, seal=base.at(2099, 1, 1)):
    """The gscore7_lowbm array; me / sic2 are this run's me_company and grp_sic2 payloads."""
    gs = np.full((len(SESSIONS), N_LINES), np.nan)
    for t in range(len(SESSIONS)):
        x = {k: np.full(N_LINES, np.nan) for k in ("bm", "roa", "cfroa", "rda", "capxa", "varroa", "varsgr")}
        cfo_gt_ni = np.zeros(N_LINES, dtype=bool)
        for i, sid in enumerate(SIDS):
            view = selected_view(rows, sid, t, seal)
            if view is None:
                continue
            r = view[-1]
            prev = me[t - 1, i] if t else math.nan
            if r["be"] > 0 and prev > 0:
                x["bm"][i] = r["be"] / prev
            if r["at"] > 0:
                x["roa"][i], x["cfroa"][i] = r["ni_ttm"] / r["at"], r["cfo_ttm"] / r["at"]
                x["capxa"][i] = r["capx_ttm"] / r["at"]
                x["rda"][i] = (r["xrd_ttm"] if math.isfinite(r["xrd_ttm"]) else 0.0) / r["at"]
            x["varroa"][i], x["varsgr"][i] = oracle_history(view)
            cfo_gt_ni[i] = r["cfo_ttm"] > r["ni_ttm"]
        m = member[t] != 0
        u = m & np.isfinite(x["bm"])
        if not u.any():
            continue
        cut = np.quantile(x["bm"][u], 1 / 3)
        for i in range(N_LINES):
            if not (u[i] and x["bm"][i] <= cut):
                continue
            keys = ("roa", "cfroa", "varroa", "varsgr", "rda", "capxa")
            if not math.isfinite(sic2[t, i]) or not all(math.isfinite(x[k][i]) for k in keys):
                continue
            peers = [p for p in range(N_LINES) if u[p] and x["bm"][p] <= cut and sic2[t, p] == sic2[t, i]]
            count = int(cfo_gt_ni[i])
            for k in keys:
                med = float(np.median([x[k][p] for p in peers if math.isfinite(x[k][p])]))
                count += int(x[k][i] < med) if k.startswith("var") else int(x[k][i] > med)
            gs[t, i] = count
    return gs


def entry(manifest, name):
    return next(e for e in manifest["fields"] if e["name"] == name)


class QuarterFields(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.w = World(Path(cls.temp.name))
        cls.manifest = cls.w.run("q")
        cls.gs = cls.w.field("q", "gscore7_lowbm")
        member = np.fromfile(cls.w.role / "member.u8", dtype="u1").reshape(len(SESSIONS), N_LINES)
        cls.member = member
        cls.want = oracle(cls.w.rows, cls.w.field("q", "me_company"), cls.w.field("q", "grp_sic2"), member)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_values_equal_the_definitions(self):
        np.testing.assert_array_equal(self.gs, self.want)
        self.assertGreater(int(np.isfinite(self.gs).sum()), 100)                # the world exercises the field
        self.assertGreater(len({float(x) for x in self.gs[np.isfinite(self.gs)]}), 3)
        for j in (13, 14):                                                     # J line, unlinked
            self.assertTrue(np.isnan(self.gs[:, j]).all(), j)
        self.assertTrue(np.isnan(self.gs[:, 10]).all())                        # be < 0: never in the bm universe
        self.assertTrue(np.isnan(self.gs[0]).all())                            # no me_company at t-1 on row 0
        self.assertTrue(np.isnan(self.gs[:5, 2]).all() and np.isnan(self.gs[20:30, 6]).all())   # non-members: NaN
        self.assertTrue(np.isfinite(self.gs[5:20, 6]).any())                   # ... and a member in the tercile
        g = entry(self.manifest, "gscore7_lowbm")
        self.assertEqual((g["fund_lag_sessions"], g["producer"]["module"]), (LAG, "research_fields_v8.py"))
        self.assertEqual((g["identity_bridge_manifest_sha256"], g["fund_events_manifest_sha256"]),
                         (self.w.bridge_sha, self.w.events_sha))
        self.assertGreater(g["member_cells"]["scored"], 100)
        self.assertEqual((g["formula_id"], g["depends_on"]), ("mohanram-g7-lowbm3-sic2-v1", ["me_company", "grp_sic2"]))
        self.assertEqual({s["field"] for s in g["sources"] if "field" in s}, {"me_company", "grp_sic2"})
        self.assertIn(self.w.events_sha, [s.get("sha256") for s in g["sources"]])
        checks = self.manifest["source_checks"]["v8"]
        self.assertEqual((checks["fund_lag_sessions"], checks["quarter_rule"]), (LAG, v8.QUARTER_RULE))
        self.assertEqual(checks["fund_events"]["rows_used"], len(self.w.rows))

    def test_field_at_t_unchanged_when_rows_after_t_mutate(self):
        cut_mark = mark(SESSIONS[CUT])
        rows = []
        for r in self.w.rows:
            if r["accepted_utc"] >= cut_mark:     # every events row at or after the t mark changes
                r = {**r, **{k: r[k] * f for k, f in (("ni_q", 1.7), ("shrs_q", 0.9), ("at", 1.3), ("be", 0.4),
                                                       ("sale_ttm", 0.6), ("ni_ttm", -1.0), ("cfo_ttm", 2.0))}}
            rows.append(r)
        new = dict(next(r for r in rows if r["cik"] == 5001 and r["period_end"] == dt.date(2020, 12, 31)))
        rows += [{**new, "accepted_utc": cut_mark, "accession": "0000005001-21-000777",          # exactly at the mark
                  "period_end": dt.date(2021, 3, 31), "ni_q": -5e6, "be": 1.0},
                 {**new, "cik": 5009, "accepted_utc": cut_mark + dt.timedelta(hours=1),
                  "accession": "0000005009-21-000777", "period_end": dt.date(2021, 3, 31), "shrs_q": 1.0}]
        sic = [(c, a, 2834 if a >= cut_mark else s, acc) for c, a, s, acc in SIC_EVENTS] + [
            (5000, cut_mark, 6022, "0000005000-21-900777")]
        mutated = World(self.w.base, rows=rows, sic_events=sic, raw_scale=1.9, name="late")   # role raw_close too
        mutated.run("late-q")
        row = N_LINES * 8
        for name in NEW:
            before = (self.w.base / "q" / f"{name}.f64").read_bytes()
            after = (self.w.base / "late-q" / f"{name}.f64").read_bytes()
            self.assertEqual(after[:(CUT + 1) * row], before[:(CUT + 1) * row], name)   # rows 0..t bit-identical
            self.assertNotEqual(after[(CUT + 1) * row:], before[(CUT + 1) * row:], name)
        me_a, me_b = self.w.field("q", "me_company"), mutated.field("late-q", "me_company")
        self.assertFalse(np.array_equal(me_a[CUT], me_b[CUT], equal_nan=True))   # me_company moved at t itself:
        member = np.fromfile(mutated.role / "member.u8", dtype="u1").reshape(len(SESSIONS), N_LINES)
        gs = oracle(rows, me_b, mutated.field("late-q", "grp_sic2"), member)   # ... gscore reads t-1 only
        np.testing.assert_array_equal(mutated.field("late-q", "gscore7_lowbm"), gs)

    def test_existing_payloads_byte_identical(self):
        alone = self.w.run("base-only", fields=BASE_FIELDS)
        for name in BASE_FIELDS:
            self.assertEqual(alone["files"][f"{name}.f64"], self.manifest["files"][f"{name}.f64"], name)
            self.assertEqual(entry(alone, name), entry(self.manifest, name), name)
        self.assertEqual({k: v for k, v in self.manifest["source_checks"].items() if k != "v8"},
                         alone["source_checks"])
        self.assertEqual([e["name"] for e in self.manifest["fields"]], BASE_FIELDS + NEW)
        self.assertEqual(list(tool.ALL_FIELDS)[-1 - len(NEW):], ["grp_ff12f49"] + NEW)   # after every other field
        self.assertFalse(set(NEW) & set(tool.DEFAULT_FIELDS))                     # opt-in
        self.assertEqual({v8.producer_group(x) for x in v8.FIELDS}, set(v8.PRODUCERS))

    def test_reuse(self):
        again = self.w.run("again", reuse=self.w.base / "q")
        self.assertEqual((again["reuse"]["reused"], again["reuse"]["computed"]), (BASE_FIELDS + NEW, []))
        self.assertEqual(again["files"], self.manifest["files"])
        self.assertEqual(again["source_checks"]["v8"], self.manifest["source_checks"]["v8"])
        for name in NEW:
            e = dict(entry(again, name))
            self.assertEqual(e.pop("reused_from")["inputs"], v8.entry_inputs(entry(self.manifest, name)), name)
            self.assertEqual(e, entry(self.manifest, name), name)
        mixed = self.w.run("mixed", reuse=self._prior_without_new())
        self.assertEqual((mixed["reuse"]["reused"], mixed["reuse"]["computed"]), (BASE_FIELDS, NEW))
        self.assertEqual(mixed["files"], self.manifest["files"])
        lag2 = self.w.run("lag2", fund_lag_sessions=2, reuse=self.w.base / "q")
        for name in NEW:
            self.assertIn("inputs differ", lag2["reuse"]["not_reused"][name])
        self.assertNotEqual(lag2["files"]["gscore7_lowbm.f64"], self.manifest["files"]["gscore7_lowbm.f64"])

    def _prior_without_new(self):
        path = self.w.base / "prior-base"
        if not path.exists():
            self.w.run("prior-base", fields=BASE_FIELDS)
        return path

    def test_refusals_before_output_and_cli(self):
        for bad in ("fund_events", "identity_bridge_sha256", "fund_lag_sessions"):
            opts = {k: v for k, v in self.w.options().items() if k != bad}
            with self.assertRaisesRegex(ValueError, "--" + bad.replace("_", "-")):
                self.w.run("refused", module_options=opts)
            self.assertFalse((self.w.base / "refused").exists())
        with self.assertRaisesRegex(ValueError, "must be an integer"):
            self.w.run("refused", module_options=self.w.options(fund_lag_sessions=9))
        with self.assertRaisesRegex(ValueError, "gscore7_lowbm requires me_company, grp_sic2"):
            self.w.run("refused", fields=["gscore7_lowbm"])
        self.assertFalse((self.w.base / "refused").exists())
        argv = ["--role", str(self.w.role), "--role-sha256", self.w.role_sha, "--output", str(self.w.base / "cli"),
                "--fields", ",".join(NEW + BASE_FIELDS), "--tickerhistory", str(self.w.th), "--finra", str(self.w.finra),
                "--identity-bridge", str(self.w.bridge), "--identity-bridge-sha256", self.w.bridge_sha,
                "--fund-events", str(self.w.events), "--fund-events-sha256", self.w.events_sha]
        with contextlib.redirect_stdout(io.StringIO()):
            tool.main(argv)                       # the builder's own arguments reach the module (OPTIONS)
        m = json.loads((self.w.base / "cli" / "manifest.json").read_bytes())
        for name in NEW:
            self.assertEqual(m["files"][f"{name}.f64"], self.manifest["files"][f"{name}.f64"], name)

    def test_seal_from_the_research_window(self):
        """A seal inside the role (the builder's SEAL_NS, which research_window sets): rows at or after it are never
        used. Rows that would move every value are added after it: the sealed output equals the world without them."""
        seal = mark(SESSIONS[CUT])
        extra = [{**r, "accepted_utc": seal + dt.timedelta(days=1, hours=j), "accession": f"{r['cik']:010d}-21-555555",
                  "period_end": dt.date(2021, 3, 31), "be": 1e3 * r["be"], "ni_ttm": -r["ni_ttm"], "ni_q": 9 * r["ni_q"]}
                 for j, r in enumerate(x for x in self.w.rows if x["period_end"] == dt.date(2020, 12, 31))]
        loud = World(self.w.base, rows=self.w.rows + extra, name="loud")
        with mock.patch.object(tool, "SEAL_NS", int(seal.timestamp()) * 10 ** 9):
            m = self.w.run("sealed")
            m_loud = loud.run("sealed-loud")
        dropped = sum(1 for r in self.w.rows if r["accepted_utc"] >= seal)
        self.assertGreater(dropped, 0)
        self.assertEqual(m["source_checks"]["v8"]["fund_events"]["rows_available_on_or_after_2025_dropped"], dropped)
        self.assertEqual(m_loud["source_checks"]["v8"]["fund_events"]["rows_available_on_or_after_2025_dropped"],
                         dropped + len(extra))
        for name in NEW:
            self.assertEqual(m_loud["files"][f"{name}.f64"], m["files"][f"{name}.f64"], name)
        unsealed = loud.run("unsealed-loud")
        for name in NEW:                          # the added rows do move the field when nothing seals them
            self.assertNotEqual(unsealed["files"][f"{name}.f64"], self.manifest["files"][f"{name}.f64"], name)
        gs = oracle(self.w.rows, self.w.field("sealed", "me_company"), self.w.field("sealed", "grp_sic2"),
                    self.member, seal=seal)
        np.testing.assert_array_equal(self.w.field("sealed", "gscore7_lowbm"), gs)


class QuarterUnits(unittest.TestCase):
    """The fiscal-quarter view and the G-score cross-section against brute force on random inputs."""

    def test_quarter_index_matches_the_rule(self):
        rng = np.random.default_rng(7)
        rows = []
        for cik in range(4):
            p = dt.date(2015, 3, 31) + dt.timedelta(days=int(rng.integers(0, 60)))
            for k in range(30):
                if rng.random() < 0.1:                                    # a missing quarter
                    p += dt.timedelta(days=91)
                    continue
                pe = None if rng.random() < 0.03 else p + dt.timedelta(days=int(rng.integers(-9, 10)))
                clock = dt.datetime(2015, 1, 1, tzinfo=UTC) + dt.timedelta(days=90 * k + int(rng.integers(40, 60)),
                                                                          hours=cik)
                rows.append({"cik": cik, "clock": clock, "acc": f"{cik}-{k:03d}", "period_end": pe, "v": rng.random()})
                if rng.random() < 0.2:                                    # an amendment of the same anchor
                    rows.append({**rows[-1], "clock": clock + dt.timedelta(days=5), "acc": f"{cik}-{k:03d}a",
                                 "v": rng.random()})
                p += dt.timedelta(days=91)
        # a tie: 2020-09-21 and 2020-10-11 are both 10 days from 2020-12-31 - 91 = 2020-10-01 -> the later one
        for clock, pe in ((dt.datetime(2020, 11, 1, tzinfo=UTC), dt.date(2020, 9, 21)),
                          (dt.datetime(2020, 11, 15, tzinfo=UTC), dt.date(2020, 10, 11)),
                          (dt.datetime(2021, 2, 10, tzinfo=UTC), dt.date(2020, 12, 31))):
            rows.append({"cik": 9, "clock": clock, "acc": f"9-{pe}", "period_end": pe, "v": 0.5})
        rows.sort(key=lambda r: (r["clock"], r["acc"]))
        cidx = np.array([r["cik"] for r in rows], dtype=np.int64)
        null = tool.NULL_PERIOD_DAY
        anchor = np.array([day(r["period_end"]) if r["period_end"] else null for r in rows], dtype=np.int64)
        index = v8.QuarterIndex({"cidx": cidx, "period_end": anchor, "known": anchor != null})
        got = index.rows(np.arange(len(rows)), 24)
        position = {id(r): i for i, r in enumerate(rows)}
        for i, r in enumerate(rows):
            view = [x for x in rows[:i + 1] if x["cik"] == r["cik"]]
            want = quarter_view(view, 24)
            for k in range(24):
                self.assertEqual(-1 if want[k] is None else position[id(want[k])], int(got[i, k]), (i, k))
        tie = next(i for i, r in enumerate(rows) if r["acc"] == "9-2020-12-31")
        self.assertEqual(rows[int(got[tie, 1])]["period_end"], dt.date(2020, 10, 11))
        self.assertGreater(int((got >= 0).sum()), 1000)
        self.assertGreater(int((got[:, 1:] < 0).sum()), 50)                     # missing quarters exercised

    def test_gscore_cross_section_matches_the_definition(self):
        rng = np.random.default_rng(11)
        n = 400
        member = rng.random(n) < 0.9
        bm = np.where(rng.random(n) < 0.1, np.nan, rng.lognormal(0, 1, n))
        sic2 = np.where(rng.random(n) < 0.05, np.nan, rng.integers(10, 18, n).astype(float))
        x = {k: np.where(rng.random(n) < 0.05, np.nan, rng.normal(0, 1, n)) for k, _ in v8.GS_SIGNALS}
        x["rda"][:50] = 0.0                                               # ties at zero (missing R&D)
        x["cfo_gt_ni"] = rng.random(n) < 0.5
        value, tercile, scored = v8.gscore_cross_section(member, bm, sic2, x)
        u = member & np.isfinite(bm)
        cut = np.quantile(bm[u], 1 / 3)
        for i in range(n):
            ok = u[i] and bm[i] <= cut
            self.assertEqual(bool(tercile[i]), ok)
            keys = [k for k, _ in v8.GS_SIGNALS]
            if not ok or not np.isfinite(sic2[i]) or not all(np.isfinite(x[k][i]) for k in keys):
                self.assertTrue(np.isnan(value[i]))
                continue
            peers = [p for p in range(n) if u[p] and bm[p] <= cut and sic2[p] == sic2[i]]
            count = int(x["cfo_gt_ni"][i])
            for k, above in v8.GS_SIGNALS:
                med = np.median([x[k][p] for p in peers if np.isfinite(x[k][p])])
                count += int(x[k][i] > med) if above else int(x[k][i] < med)
            self.assertEqual(value[i], count, i)
        self.assertEqual(int(scored.sum()), int(np.isfinite(value).sum()))
        self.assertGreater(int(scored.sum()), 60)


if __name__ == "__main__":
    unittest.main()
