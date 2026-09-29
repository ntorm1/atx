"""research_fields_holdings (platform v7 W5b): synthetic stage fixtures, no real archive access.

13F cells are checked against hand-computed plants (effective filings, the 45-day deadline clock, a quarter without a
row, the two-manager best-ideas arithmetic, unit and price-outlier screens); FTD, Reg SHO and short-volume-ext are
checked against brute-force reference implementations. The byte-identity test runs the builder's full recipe with and
without the holdings fields.
"""
import contextlib
import datetime as dt
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import prepare_research_fields as tool
import research_fields_holdings as hold
import test_prepare_research_fields as base

UTC = dt.timezone.utc
IDS = base.IDS                                   # 101 202 303 404 505
SESSIONS = [d for d in (dt.date(2024, 5, 1) + dt.timedelta(days=i) for i in range(245)) if d.weekday() < 5]
Q0, Q1, Q2, Q3 = dt.date(2023, 12, 31), dt.date(2024, 3, 31), dt.date(2024, 6, 30), dt.date(2024, 9, 30)
DEADLINE = {Q0: dt.date(2024, 2, 14), Q1: dt.date(2024, 5, 15), Q2: dt.date(2024, 8, 14), Q3: dt.date(2024, 11, 14)}
CUSIP = {"C101": 101, "C202": 202, "C303": 303}
A, B, C = "1", "2", "3"
# (accession, filer, period, filing date, form, amendment type, source part, holdings rows (cusip, shares, value_usd))
FILINGS = [
    ("a-q0", A, Q0, dt.date(2024, 2, 1), "13F-HR", None, "2024q1", [("C101", 100, 1000)]),
    ("b-q0", B, Q0, dt.date(2024, 2, 14), "13F-HR", None, "2024q1", [("C101", 50, 500)]),
    ("a-q1", A, Q1, dt.date(2024, 4, 20), "13F-HR", None, "2024q2", [("C101", 100, 1000), ("C202", 40, 400),
                                                                     ("CXXX", 60, 600)]),
    ("b-q1", B, Q1, dt.date(2024, 5, 15), "13F-HR", None, "2024q2", [("C202", 60, 600)]),
    ("c-q1", C, Q1, dt.date(2024, 5, 16), "13F-HR", None, "2024q2", [("C101", 9, 90)]),          # after the deadline
    ("a-q2", A, Q2, dt.date(2024, 7, 15), "13F-HR", None, "2024q3", [("C101", 6, 60), ("C202", 4, 40)]),
    ("b-q2", B, Q2, dt.date(2024, 7, 20), "13F-HR", None, "2024q3", [("C101", 99.9, 999)]),     # restated below
    ("b-q2r", B, Q2, dt.date(2024, 8, 14), "13F-HR/A", "RESTATEMENT", "2024q3", [("C101", 10, 100), ("C303", 30, 300)]),
    ("b-q2x", B, Q2, dt.date(2024, 8, 1), "13F-HR/A", None, "2024q3", [("C202", 1, 10)]),       # untyped: not used
    ("a-q3", A, Q3, dt.date(2024, 10, 10), "13F-HR", None, "2024q4", [("C101", 7, 70)]),
    ("a-q3n", A, Q3, dt.date(2024, 11, 1), "13F-HR/A", "NEW HOLDINGS", "2024q4", [("C303", 3, 30)]),
    ("b-q3", B, Q3, dt.date(2024, 11, 14), "13F-HR", None, "2024q4", [("C101", 20, 200_000)]),  # dollars: factor .001
    ("c-q3", C, Q3, dt.date(2024, 10, 20), "13F-HR", None, "2024q4", [("C101", 1, 1000)]),      # price outlier
    ("a-q3r", A, Q3, dt.date(2024, 12, 1), "13F-HR/A", "RESTATEMENT", "2024q4", [("C202", 5, 50)]),  # late: ignored
    ("n-q3", "4", Q3, dt.date(2024, 11, 14), "13F-NT", None, "2024q4", []),                     # notice: clock only
]
UNIT = {"b-q3": 0.001}
# The stage aggregate (independent plants): (period, security, inst_shares, n_holders)
AGG = [(Q0, 101, 150, 2), (Q1, 101, 300_000, 1), (Q1, 202, 500_000, 2), (Q2, 101, 400_000, 2), (Q2, 202, 600_000, 1),
       (Q2, 303, 100_000, 1), (Q3, 101, 450_000, 2), (Q3, 303, 3_000_000, 1)]
FTD_END = dt.date(2024, 9, 30)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def instant(d, hours=0):
    return dt.datetime(d.year, d.month, d.day, tzinfo=UTC) + dt.timedelta(hours=hours)


def weekdays(a, b):
    return [a + dt.timedelta(days=i) for i in range((b - a).days + 1) if (a + dt.timedelta(days=i)).weekday() < 5]


def write_stage(root: Path, key: str, tables: dict, **extra) -> str:
    root.mkdir(parents=True)
    files = {}
    for rel, table in tables.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        pq.write_table(table, path)
        files[rel] = {"bytes": path.stat().st_size, "sha256": sha(path)}
    rules = {k: v for k, v in hold.STAGES[key].items() if k != "schema"}
    manifest = {"schema": hold.STAGES[key]["schema"], "status": "complete", **rules, "files": files, **extra}
    (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return sha(root / "manifest.json")


def ts(values):
    return pa.array(values, pa.timestamp("us", tz="UTC"))


def write_thirteenf(root: Path, drop_agg=()):
    f = {k: [] for k in ("accession", "filer_cik", "period_q", "filing_date", "submission_type", "amendment_type",
                         "available_at", "deadline", "source_period")}
    parts = {}
    for acc, filer, q, fd, form, amend, part, rows in FILINGS:
        for k, v in zip(f, (acc, filer, q, fd, form, amend, instant(fd, 46), DEADLINE[q], part)):
            f[k].append(v)
        for cusip, shares, value in rows:
            h = parts.setdefault(part, {k: [] for k in ("accession", "cusip", "shares", "sshprnamt_type", "put_call",
                                                          "value_usd")})
            for k, v in zip(h, (acc, cusip, float(shares), "SH", None, float(value))):
                h[k].append(v)
    # rows every screen must drop: an option, a principal amount and a zero-share row on a used filing
    h = parts["2024q3"]
    for k, v in zip(h, ("a-q2", "C303", 5.0, "SH", "CALL", 50.0)):
        h[k].append(v)
    for k, v in zip(h, ("a-q2", "C303", 5.0, "PRN", None, 50.0)):
        h[k].append(v)
    for k, v in zip(h, ("a-q2", "C303", 0.0, "SH", None, 50.0)):
        h[k].append(v)
    tables = {"filings.parquet": pa.table({
        "accession": f["accession"], "filer_cik": f["filer_cik"], "period_q": pa.array(f["period_q"], pa.date32()),
        "filing_date": pa.array(f["filing_date"], pa.date32()), "submission_type": f["submission_type"],
        "amendment_type": pa.array(f["amendment_type"], pa.string()), "available_at": ts(f["available_at"]),
        "deadline": pa.array(f["deadline"], pa.date32()), "source_period": f["source_period"]})}
    for part, h in parts.items():
        tables[f"parts/source={part}/holdings.parquet"] = pa.table({
            "accession": h["accession"], "cusip": h["cusip"], "shares": pa.array(h["shares"], pa.float64()),
            "sshprnamt_type": h["sshprnamt_type"], "put_call": pa.array(h["put_call"], pa.string()),
            "value_usd": pa.array(h["value_usd"], pa.float64())})
    tables["filing_checks.parquet"] = pa.table({"accession": list(UNIT), "unit_factor": list(UNIT.values())})
    cm = [(q, c, s) for q in (Q0, Q1, Q2, Q3) for c, s in CUSIP.items()]
    tables["cusip_map_pit.parquet"] = pa.table({"period_q": pa.array([x[0] for x in cm], pa.date32()),
                                                "cusip": [x[1] for x in cm], "security_id": [x[2] for x in cm]})
    agg = [a for a in AGG if (a[0], a[1]) not in drop_agg]
    tables["agg_asof45.parquet"] = pa.table({
        "period_of_report": pa.array([a[0] for a in agg], pa.date32()), "security_id": [a[1] for a in agg],
        "inst_shares": pa.array([float(a[2]) for a in agg]), "n_holders": [a[3] for a in agg],
        "available_at": ts([instant(DEADLINE[a[0]], 46) for a in agg]), "version": ["asof45"] * len(agg)})
    return write_stage(root, "thirteenf", tables)


def ftd_available(d):
    if d.day <= 14:
        end = (d.replace(day=28) + dt.timedelta(days=4)).replace(day=1) - dt.timedelta(days=1)
        return instant(end + dt.timedelta(days=7))
    nxt = (d.replace(day=28) + dt.timedelta(days=4)).replace(day=15)
    return instant(nxt + dt.timedelta(days=7))


def ftd_rows():
    rows = []  # (settlement, security_id or None, qty, map_basis)
    for d in weekdays(dt.date(2024, 1, 2), FTD_END):
        rows.append((d, 101, 1000.0 + d.day, "exact"))
        rows.append((d, 303, 100.0, "exact"))
        rows.append((d, 303, 9999.0, "collision"))
        rows.append((d, None, 5.0, "unmapped"))
        if d.month == 6:
            rows.append((d, 202, 500.0, "canonical"))
        if d == dt.date(2024, 7, 1):
            rows.append((d, 303, 50.0, "exact"))    # a second CUSIP of the line: summed
    return rows


def write_ftd(root: Path):
    rows = ftd_rows()
    table = pa.table({"settlement_date": pa.array([r[0] for r in rows], pa.date32()),
                      "security_id": pa.array([r[1] for r in rows], pa.int64()),
                      "quantity": [r[2] for r in rows], "available_at": ts([ftd_available(r[0]) for r in rows]),
                      "map_basis": [r[3] for r in rows]})
    return write_stage(root, "ftd", {"year=2024/ftd.parquet": table})


LIST_DAYS = weekdays(dt.date(2024, 1, 2), dt.date(2024, 12, 31))
NASDAQ_ABSENT = {dt.date(2024, 9, 16), dt.date(2024, 9, 17), dt.date(2024, 9, 18), dt.date(2024, 9, 19)}
ON_LIST = {101: [d for d in LIST_DAYS if dt.date(2024, 6, 3) <= d <= dt.date(2024, 6, 28)],
           303: [d for d in LIST_DAYS if d >= dt.date(2024, 11, 1)],
           202: [d for d in LIST_DAYS if d.month == 7]}
MARKET_CLASS = {101: "NNM", 202: "NYSE", 303: "SC", 404: "IEX"}


def list_available(market, d):
    if market in ("nasdaq", "nyse"):
        return instant(d + dt.timedelta(days=1), 6)
    nxt = d + dt.timedelta(days=1)
    while nxt.weekday() >= 5:
        nxt += dt.timedelta(days=1)
    return instant(nxt, 16)


def write_regsho(root: Path):
    lists = []
    for d in LIST_DAYS:
        lists.append(("nasdaq", d, "absent" if d in NASDAQ_ABSENT else ("empty_list" if d.day == 5 else "list")))
        lists.append(("cboe_bzx", d, "list"))
        if d < dt.date(2024, 1, 20):
            lists.append(("nyse", d, "list"))
    ltab = pa.table({"market": [x[0] for x in lists], "list_date": pa.array([x[1] for x in lists], pa.date32()),
                     "status": [x[2] for x in lists], "available_at": ts([list_available(x[0], x[1]) for x in lists])})
    rows = []
    for sid, days in ON_LIST.items():
        market = "nyse" if sid == 202 else "nasdaq"
        rows += [(d, market, sid, True) for d in days if not (market == "nasdaq" and d in NASDAQ_ABSENT)]
    rows.append((dt.date(2024, 6, 3), "nasdaq", 303, False))       # an off-list row: never counted
    rows.append((dt.date(2024, 6, 4), "nasdaq", None, True))       # unmapped
    ttab = pa.table({"list_date": pa.array([r[0] for r in rows], pa.date32()), "market": [r[1] for r in rows],
                     "security_id": pa.array([r[2] for r in rows], pa.int64()), "on_list": [r[3] for r in rows],
                     "available_at": ts([list_available(r[1], r[0]) for r in rows])})
    return write_stage(root, "regsho_threshold", {"lists.parquet": ltab, "year=2024/threshold.parquet": ttab})


def name_rows():
    out = []
    for sid, mc in MARKET_CLASS.items():
        for d in [dt.date(2023, 12, 10) + dt.timedelta(days=15 * k) for k in range(26)]:
            if sid == 303 and d > dt.date(2024, 9, 1):
                continue                                               # stale after 45 days: market unknown
            out.append((sid, d, mc))
    return out


def write_security_master(root: Path):
    rows = name_rows()
    table = pa.table({"security_id": [r[0] for r in rows], "dissemination_date": pa.array([r[1] for r in rows], pa.date32()),
                      "available_at": pa.array([dt.datetime(r[1].year, r[1].month, r[1].day, 22) for r in rows],
                                               pa.timestamp("us")),
                      "market_class": [r[2] for r in rows]})
    return write_stage(root, "security_master", {"finra_names.parquet": table})


def svx_rows():
    rows = []  # (security_id, trade_date, short, total)
    for d in weekdays(dt.date(2024, 3, 1), dt.date(2024, 12, 31)):
        rows.append((101, d, 2000.0 + d.day, 4000.0))
        if d.day % 3:
            rows.append((202, d, 3000.0, 5000.0))
        rows.append((303, d, 20000.0, 30000.0))       # above the consolidated volume: outside [0, 1]
        rows.append((None, d, 1.0, 1.0))
    return rows


def write_svx(root: Path):
    rows = svx_rows()
    table = pa.table({"security_id": pa.array([r[0] for r in rows], pa.int64()),
                      "trade_date": pa.array([r[1] for r in rows], pa.date32()),
                      "short_volume": [r[2] for r in rows], "total_volume": [r[3] for r in rows],
                      "available_at": ts([instant(r[1] + dt.timedelta(days=1)) for r in rows])})
    return write_stage(root, "short_volume_ext", {"year=2024/short_volume_ext.parquet": table})


class HoldFixture(base.Fixture):
    def __init__(self, root: Path):
        self.base = root
        self.th = root / "TickerHistory3.parquet"
        base.write_tickerhistory(self.th)
        self.finra = root / "finra"
        base.write_finra(self.finra)
        self.lake = root / "lake"
        base.write_lake(self.lake)
        self.role = root / "role"
        self.role_sha = base.write_role(self.role, sha(self.th), SESSIONS)
        self.stages = {}
        for key, writer in (("thirteenf", write_thirteenf), ("ftd", write_ftd), ("regsho_threshold", write_regsho),
                            ("security_master", write_security_master), ("short_volume_ext", write_svx)):
            path = root / key
            self.stages[key] = path
            self.stages[key + "_sha256"] = writer(path)

    def run(self, out, **kw):
        for k, v in self.stages.items():
            kw.setdefault(k, v)
        return super().run(out, **kw)

    def field(self, out, name):
        return np.fromfile(self.base / out / f"{name}.f64", dtype="<f8").reshape(len(SESSIONS), len(IDS))


def t_of(d):
    return SESSIONS.index(d)


def col(sid):
    return IDS.index(sid)


ALL = list(hold.HOLD_FIELDS)
RECIPE = ["si_shares", "shares_out"] + ALL


class Pure(unittest.TestCase):
    def test_effective_filings(self):
        sel = [x for x in FILINGS if x[3] <= DEADLINE[x[2]] and x[4] in hold.F13_FORMS]
        eff = hold.effective_filings([hold.day_of(x[2]) for x in sel], [int(x[1]) for x in sel],
                                     [hold.day_of(x[3]) for x in sel], [x[0] for x in sel], [x[4] for x in sel],
                                     [x[5] or "" for x in sel])
        got = sorted(x[0] for x, e in zip(sel, eff) if e)
        self.assertEqual(got, sorted(["a-q0", "b-q0", "a-q1", "b-q1", "a-q2", "b-q2r", "a-q3", "a-q3n", "b-q3", "c-q3"]))
        # a NEW HOLDINGS amendment before a later restatement is superseded by it
        eff = hold.effective_filings([1, 1, 1], [7, 7, 7], [10, 11, 12], ["x1", "x2", "x3"],
                                     ["13F-HR", "13F-HR/A", "13F-HR/A"], ["", "NEW HOLDINGS", "RESTATEMENT"])
        self.assertEqual(eff.tolist(), [False, False, True])

    def test_best_ideas_two_managers(self):
        # A: 101 60, 202 40 (V 100); B: 101 100, 303 300 (V 400); mw = 101 .32, 202 .08, 303 .6
        r = hold.quarter_holdings([1, 1, 2, 2], [101, 202, 101, 303], [6, 4, 10, 30], [60, 40, 100, 300])
        self.assertEqual(r["sids"].tolist(), [101, 202, 303])
        np.testing.assert_allclose(r["best"], [0.28, 0.32, 0.15], rtol=0, atol=1e-12)
        self.assertEqual(r["holders"].tolist(), [2, 1, 1])
        # an unmapped row enters its manager's total only; a 100x implied price is screened out everywhere
        r = hold.quarter_holdings([1, 1, 2, 3], [101, -1, 101, 101], [6, 1, 10, 1], [60, 40, 100, 1000])
        np.testing.assert_allclose(r["best"], [0.0 + 0.0], atol=1e-12)
        self.assertEqual((r["rows_price_outlier"], r["filers"].tolist()), (1, [1, 2]))

    def test_breadth_change(self):
        prev = hold.quarter_holdings([1, 2], [101, 202], [1, 1], [10, 10])
        cur = hold.quarter_holdings([1, 1, 3], [101, 202, 303], [1, 1, 1], [10, 10, 10])
        # continuing filers {1} (2 left, 3 entered): 101 1-1, 202 1-0, 303 not in prev -> NaN
        np.testing.assert_array_equal(hold.breadth_change(cur, prev), [0.0, 1.0, np.nan])
        self.assertTrue(np.all(np.isnan(hold.breadth_change(cur, None))))

    def test_anchor_forward_fill_and_staleness(self):
        pdays = np.array([100, 191], dtype=np.int64)
        vis = np.array([150, 240], dtype=np.int64) * hold.DAY_NS
        has = np.array([[True, True], [True, False]])
        self.assertEqual(hold.anchor_quarters(pdays, vis, has, 245, 241 * hold.DAY_NS).tolist(), [1, 0])
        self.assertEqual(hold.anchor_quarters(pdays, vis, has, 251, 250 * hold.DAY_NS).tolist(), [1, -1])  # > 150 d
        self.assertEqual(hold.anchor_quarters(pdays, vis, has, 241, 240 * hold.DAY_NS).tolist(), [0, 0])   # strict <

    def test_quarter_helpers(self):
        self.assertEqual(hold.quarter_end(dt.date(2024, 2, 29)), Q1)
        self.assertEqual(hold.prev_quarter_end(Q1), Q0)
        self.assertEqual(hold.needed_quarters(SESSIONS[0], SESSIONS[-1]),
                         [dt.date(2023, 9, 30), Q0, Q1, Q2, Q3])


class HoldingsFields(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.fx = HoldFixture(Path(cls.temp.name))
        cls.manifest = cls.fx.run("fields", fields=RECIPE)
        cls.f = {x: cls.fx.field("fields", x) for x in ALL}
        cls.so = cls.fx.field("fields", "shares_out")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def cell(self, name, d, sid):
        return self.f[name][t_of(d), col(sid)]

    def test_13f_visibility_at_the_deadline_clock(self):
        # Q1's latest filing by the deadline (2024-05-15) gives V = 05-16 22:00: NOT visible on 05-17 (strict), visible 05-20
        self.assertEqual(self.cell("inst_n_holders", dt.date(2024, 5, 17), 101), 2)   # still Q0
        self.assertEqual(self.cell("inst_n_holders", dt.date(2024, 5, 20), 101), 1)   # Q1
        self.assertTrue(np.isnan(self.cell("inst_n_holders", dt.date(2024, 5, 17), 202)))
        self.assertEqual(self.cell("inst_n_holders", dt.date(2024, 5, 20), 202), 2)
        self.assertTrue(np.isnan(self.cell("inst_n_holders", dt.date(2024, 8, 16), 303)))  # Q2 V = 08-15 22:00
        self.assertEqual(self.cell("inst_n_holders", dt.date(2024, 8, 19), 303), 1)
        self.assertEqual(self.cell("inst_n_holders", dt.date(2024, 11, 18), 101), 2)
        self.assertEqual(self.cell("inst_own_share", dt.date(2024, 11, 18), 101), 0.4)   # Q2 until Q3 is visible
        self.assertEqual(self.cell("inst_own_share", dt.date(2024, 11, 19), 101), 0.45)  # Q3 (V = 11-15 22:00, Fri)
        clock = self.manifest["source_checks"]["holdings"]["thirteenf"]["quarter_visible_at"]
        self.assertEqual(clock[Q1.isoformat()], "2024-05-16T22:00:00.000000000Z")   # c-q1 (after the deadline) excluded
        self.assertIsNone(clock["2023-09-30"])

    def test_13f_forward_fill_across_a_missing_quarter(self):
        # 202 has no Q3 row: Q2 carries on after Q3 is visible, until 150 days after 2024-06-30 (2024-11-27)
        for d in (dt.date(2024, 11, 19), dt.date(2024, 11, 27)):
            self.assertEqual(self.cell("inst_own_share", d, 202), 0.3)
            self.assertEqual(self.cell("inst_n_holders", d, 202), 1)
            self.assertAlmostEqual(self.cell("inst_best_ideas", d, 202), 0.32)
        for x in ALL[:5]:
            self.assertTrue(np.isnan(self.cell(x, dt.date(2024, 11, 28), 202)), x)
        e = next(e for e in self.manifest["fields"] if e["name"] == "inst_n_holders")
        self.assertGreater(e["nan_reasons_member_cells"]["forward_filled_finite"], 0)

    def test_13f_values(self):
        d2, d3 = dt.date(2024, 9, 3), dt.date(2024, 12, 2)
        # Q1 (2024-03-31) precedes the role: no quarter-end session, ownership NaN; its holdings fields exist
        self.assertTrue(np.isnan(self.cell("inst_own_share", dt.date(2024, 6, 3), 101)))
        self.assertAlmostEqual(self.cell("inst_best_ideas", dt.date(2024, 6, 3), 202), 0.5)   # unmapped row in A's total
        self.assertAlmostEqual(self.cell("inst_best_ideas", dt.date(2024, 6, 3), 101), 0.0)
        # Q2: the planted two-manager case (B restated; the untyped 13F-HR/A ignored)
        for sid, best in ((101, 0.28), (202, 0.32), (303, 0.15)):
            self.assertAlmostEqual(self.cell("inst_best_ideas", d2, sid), best, places=12)
        for sid, io in ((101, 0.4), (202, 0.3), (303, 0.2)):
            self.assertEqual(self.cell("inst_own_share", d2, sid), io)
        # breadth Q2 vs Q1 over continuing filers {A, B}: 101 {A,B} vs {A} -> +1/2; 202 {A} vs {A,B} -> -1/2
        self.assertEqual(self.cell("inst_breadth_chg", d2, 101), 0.5)
        self.assertEqual(self.cell("inst_breadth_chg", d2, 202), -0.5)
        self.assertTrue(np.isnan(self.cell("inst_breadth_chg", d2, 303)))                  # no Q1 row
        # Q3: B's dollars corrected by the unit factor, C's 100x price screened, A's NEW HOLDINGS added, the late
        # restatement ignored: mw 101 .9 / 303 .1; best 101 = (1 - .9) = .1, 303 = .3 - .1 = .2
        self.assertAlmostEqual(self.cell("inst_best_ideas", d3, 101), 0.1, places=12)
        self.assertAlmostEqual(self.cell("inst_best_ideas", d3, 303), 0.2, places=12)
        self.assertEqual(self.cell("inst_breadth_chg", d3, 101), 0.0)
        self.assertAlmostEqual(self.cell("inst_own_chg_q", d3, 101), 0.05, places=12)
        # 303: 3e6 / 5e5 = 6 > the declared maximum 2 -> NaN (and so its change); counted
        self.assertTrue(np.isnan(self.cell("inst_own_share", d3, 303)))
        self.assertTrue(np.isnan(self.cell("inst_own_chg_q", d3, 303)))
        e = next(e for e in self.manifest["fields"] if e["name"] == "inst_own_share")
        self.assertEqual(e["nan_reasons_member_cells"]["quarter_cells_above_declared_max_2"], 1)
        self.assertTrue(np.isnan(self.cell("inst_own_chg_q", d2, 101)))                    # Q1 IO is NaN
        h = self.manifest["source_checks"]["holdings"]["thirteenf"]
        self.assertEqual(h["effective_filings"], 10)
        self.assertEqual((h["filings_after_deadline_ignored"], h["hr_a_without_amendment_type_ignored"]), (2, 1))
        self.assertEqual(h["holdings_per_quarter"][Q3.isoformat()]["rows_price_outlier"], 1)
        self.assertEqual(h["quarters_without_filings"], ["2023-09-30"])

    def test_13f_every_value_uses_a_visible_quarter(self):
        # point in time: removing the Q3 agg rows changes no cell before Q3 is visible
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            stages = dict(self.fx.stages)
            stages["thirteenf"] = root / "13f"
            stages["thirteenf_sha256"] = write_thirteenf(root / "13f", drop_agg={(Q3, 101), (Q3, 303)})
            m = self.fx.run(Path(temp) / "out", fields=["si_shares", "shares_out", "inst_own_share", "inst_n_holders"],
                            **stages)
            self.assertEqual(len(m["fields"]), 4)
            cut = t_of(dt.date(2024, 11, 19))
            for x in ("inst_own_share", "inst_n_holders"):
                got = np.fromfile(root / "out" / f"{x}.f64", dtype="<f8").reshape(len(SESSIONS), len(IDS))
                np.testing.assert_array_equal(got[:cut], self.f[x][:cut], x)
                self.assertFalse(np.array_equal(got[cut:], self.f[x][cut:], equal_nan=True), x)

    def ftd_expected(self):
        rows = ftd_rows()
        cal = sorted({r[0] for r in rows})
        avail = {d: max(ftd_available(r[0]) for r in rows if r[0] == d) for d in cal}
        qty = {}
        for d, sid, q, basis in rows:
            if sid is not None and basis != "collision":
                qty[(d, sid)] = qty.get((d, sid), 0.0) + q
        out = np.full((len(SESSIONS), len(IDS)), np.nan)
        for t in range(1, len(SESSIONS)):
            mark = instant(SESSIONS[t - 1], 22)
            vis = []
            for d in cal:                              # the prefix of settlement dates that is entirely visible
                if avail[d] >= mark:
                    break
                vis.append(d)
            if not vis:
                continue
            star = vis[-1]
            if (SESSIONS[t] - star).days > 60 or cal.index(star) < 20:
                continue
            window = cal[cal.index(star) - 20: cal.index(star) + 1]
            ts_ = max(k for k, s in enumerate(SESSIONS) if s <= star) if SESSIONS[0] <= star else None
            for i, sid in enumerate(IDS):
                so = self.so[ts_, i] if ts_ is not None else np.nan
                if np.isfinite(so) and so > 0:
                    out[t, i] = sum(qty.get((d, sid), 0.0) for d in window) / so
        return out

    def test_ftd_against_brute_force(self):
        np.testing.assert_allclose(self.f["ftd_shares_ratio21"], self.ftd_expected(), rtol=1e-12, equal_nan=True)
        got = self.f["ftd_shares_ratio21"]
        self.assertTrue(np.isfinite(got[t_of(dt.date(2024, 11, 29)), col(101)]))
        self.assertTrue(np.isnan(got[t_of(dt.date(2024, 12, 2)), col(101)]))    # > 60 days after 2024-09-30
        self.assertTrue(np.isfinite(got[t_of(dt.date(2024, 7, 23)), col(202)]))  # 0 fails outside June: finite
        c = self.manifest["source_checks"]["holdings"]["ftd"]
        self.assertEqual(c["role_rows_summed_duplicates"], 1)

    def regsho_expected(self):
        cal = [d for d in LIST_DAYS if d < SESSIONS[0]][-(hold.REGSHO_WINDOW + 2):] + SESSIONS
        status = {}
        for d in LIST_DAYS:
            if d not in NASDAQ_ABSENT:
                status[("nasdaq", d)] = list_available("nasdaq", d)
            status[("cboe_bzx", d)] = list_available("cboe_bzx", d)
            if d < dt.date(2024, 1, 20):
                status[("nyse", d)] = list_available("nyse", d)
        names = name_rows()
        out = np.full((len(SESSIONS), len(IDS)), np.nan)
        for t in range(1, len(SESSIONS)):
            mark = instant(SESSIONS[t - 1], 22)
            e = cal.index(SESSIONS[t]) - 2
            window = cal[e - 62: e + 1]
            for i, sid in enumerate(IDS):
                rows = [r for r in names if r[0] == sid and dt.datetime(r[1].year, r[1].month, r[1].day, 22,
                                                                        tzinfo=UTC) < mark]
                if not rows or (SESSIONS[t] - rows[-1][1]).days > 45:
                    continue
                market = hold.REGSHO_MARKET_OF_CLASS.get(rows[-1][2])
                if market is None:
                    continue
                if sum(1 for d in window if status.get((market, d), dt.datetime.max.replace(tzinfo=UTC)) < mark) < 60:
                    continue
                lm = "nyse" if sid == 202 else "nasdaq"
                out[t, i] = sum(1 for d in window if d in ON_LIST.get(sid, []) and (lm, d) in status
                                and status[(lm, d)] < mark)
        return out

    def test_regsho_against_brute_force(self):
        got = self.f["regsho_threshold_days63"]
        np.testing.assert_array_equal(got, self.regsho_expected())
        self.assertEqual(got[t_of(dt.date(2024, 7, 2)), col(101)], 20)          # 06-03..06-28, all visible
        self.assertTrue(np.all(np.isnan(got[:, col(202)])))                     # NYSE lists absent: never a false 0
        self.assertTrue(np.all(np.isnan(got[:, col(404)])))                     # IEX: no list source
        self.assertTrue(np.isnan(got[t_of(dt.date(2024, 10, 1)), col(101)]))     # 4 absent Nasdaq lists in the window
        self.assertEqual(got[-1, col(101)], 0)                                   # a complete window: a true zero
        self.assertTrue(np.isnan(got[-1, col(303)]))                             # name row older than 45 days
        c = self.manifest["source_checks"]["holdings"]["regsho_threshold"]
        self.assertEqual(c["finite_member_cells_by_listing_market"]["nyse"], 0)

    def svx_expected(self):
        rows = {(r[0], r[1]): r[2] for r in svx_rows() if r[0] is not None}
        close, raw, present = base.role_prices(len(SESSIONS))
        out = np.full((len(SESSIONS), len(IDS)), np.nan)
        for t in range(1, len(SESSIONS)):
            if t - 2 - 126 + 1 < 0:
                continue
            for i, sid in enumerate(IDS):
                s = v = n = 0
                for k in range(t - 127, t - 1):
                    if (sid, SESSIONS[k]) in rows and present[k, i]:
                        s += rows[(sid, SESSIONS[k])]
                        v += 1e4
                        n += 1
                if n >= 63 and v > 0 and 0 <= s / v <= 1:
                    out[t, i] = s / v
        return out

    def test_short_volume_ext_against_brute_force(self):
        np.testing.assert_allclose(self.f["sv_offexchange_share126"], self.svx_expected(), rtol=1e-12, equal_nan=True)
        got = self.f["sv_offexchange_share126"]
        self.assertTrue(np.all(np.isnan(got[:, col(303)])))                     # > 1: outside the declared domain
        self.assertTrue(np.isfinite(got[-1, col(202)]))
        c = self.manifest["source_checks"]["holdings"]["short_volume_ext"]
        self.assertEqual(c["finra_total_over_vendor_volume_member_cells"]["2024"] > 0, True)

    def test_manifest_entries(self):
        m = self.manifest
        self.assertEqual([e["name"] for e in m["fields"]], RECIPE)
        for e in m["fields"][2:]:
            spec = hold.HOLD_FIELDS[e["name"]]
            self.assertEqual(e["formula_id"], spec["formula_id"])
            self.assertTrue(e["point_in_time"])
            self.assertEqual(e["sha256"], sha(self.fx.base / "fields" / e["file"]))
            self.assertEqual(m["files"][e["file"]]["sha256"], e["sha256"])
            for key, pin in e["stage_manifest_sha256"].items():
                self.assertEqual(pin, self.fx.stages[key + "_sha256"])
                self.assertIn(pin, [s["sha256"] for s in e["sources"]])
            self.assertEqual(set(e["coverage"]["per_year"]), {"2024"})
            self.assertIn("member_finite_quantiles", e["coverage"])
            self.assertEqual(e.get("depends_on", []), spec.get("requires", []))
        h = m["source_checks"]["holdings"]
        self.assertEqual(h["code"]["code_sha256"], sha(hold.__file__))
        self.assertEqual(set(h["stages"]), {"thirteenf", "ftd", "regsho_threshold", "security_master",
                                            "short_volume_ext"})

    def test_refusals(self):
        with self.assertRaisesRegex(ValueError, "requires shares_out"):
            self.fx.run("r1", fields=["inst_own_share"])
        with self.assertRaisesRegex(ValueError, "needs --ftd"):
            self.fx.run("r2", fields=["si_shares", "shares_out", "ftd_shares_ratio21"], ftd=None)
        with self.assertRaisesRegex(ValueError, "does not match --thirteenf-sha256"):
            self.fx.run("r3", fields=["inst_n_holders"], thirteenf_sha256="0" * 64)
        self.assertFalse((self.fx.base / "r3" / "manifest.json").exists())
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            bad = root / "ftd"
            sha_bad = write_stage(bad, "ftd", {"year=2024/ftd.parquet": pq.read_table(self.fx.stages["ftd"] /
                                                                                      "year=2024/ftd.parquet")})
            m = json.loads((bad / "manifest.json").read_bytes())
            m["staleness_days"] = 90
            (bad / "manifest.json").write_text(json.dumps(m), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "staleness_days"):
                self.fx.run(root / "o1", fields=["si_shares", "shares_out", "ftd_shares_ratio21"], ftd=bad,
                            ftd_sha256=sha(bad / "manifest.json"))
            self.assertNotEqual(sha_bad, sha(bad / "manifest.json"))
            tampered = root / "svx"
            write_svx(tampered)
            path = tampered / "year=2024" / "short_volume_ext.parquet"
            path.write_bytes(path.read_bytes() + b"\0")
            with self.assertRaisesRegex(ValueError, "does not match its stage manifest"):
                self.fx.run(root / "o2", fields=["sv_offexchange_share126"], short_volume_ext=tampered,
                            short_volume_ext_sha256=sha(tampered / "manifest.json"))
            self.assertFalse((root / "o2" / "manifest.json").exists())

    def test_cli(self):
        s = self.fx.stages
        with contextlib.redirect_stdout(io.StringIO()):
            tool.main(["--role", str(self.fx.role), "--role-sha256", self.fx.role_sha,
                       "--output", str(self.fx.base / "cli"), "--fields", "inst_n_holders,regsho_threshold_days63",
                       "--finra", str(self.fx.finra), "--tickerhistory", str(self.fx.th), "--lake", str(self.fx.lake),
                       "--thirteenf", str(s["thirteenf"]), "--thirteenf-sha256", s["thirteenf_sha256"],
                       "--regsho-threshold", str(s["regsho_threshold"]),
                       "--regsho-threshold-sha256", s["regsho_threshold_sha256"],
                       "--security-master", str(s["security_master"]),
                       "--security-master-sha256", s["security_master_sha256"]])
        m = json.loads((self.fx.base / "cli" / "manifest.json").read_bytes())
        self.assertEqual([e["name"] for e in m["fields"]], ["inst_n_holders", "regsho_threshold_days63"])
        self.assertEqual(sorted(p.name for p in (self.fx.base / "cli").iterdir()),
                         ["inst_n_holders.f64", "manifest.json", "regsho_threshold_days63.f64"])
        for x in ("inst_n_holders", "regsho_threshold_days63"):   # holdings-only (carried run) == full run bytes
            self.assertEqual(m["files"][f"{x}.f64"], self.manifest["files"][f"{x}.f64"])


class ByteIdentity(unittest.TestCase):
    """Every other field's bytes, manifest entry and source check are identical with and without the holdings fields."""

    def test_full_recipe_with_and_without_holdings(self):
        with tempfile.TemporaryDirectory() as temp:
            fx = HoldFixture(Path(temp))
            bridge, events = fx.base / "bridge", fx.base / "events"
            kw = dict(identity_bridge=bridge, identity_bridge_sha256=base.write_bridge(bridge),
                      fund_events=events, fund_events_sha256=base.write_events(events))
            recipe = list(tool.FIELDS) + list(tool.ISSUER_FIELDS)
            without = base.Fixture.run(fx, "without", fields=recipe, **kw)
            with_h = fx.run("with", fields=recipe + ALL, **kw)
            self.assertEqual([e["name"] for e in with_h["fields"]], recipe + ALL)
            for name in recipe:
                a, b = (fx.base / "without" / f"{name}.f64").read_bytes(), (fx.base / "with" / f"{name}.f64").read_bytes()
                self.assertEqual(a, b, name)
                self.assertEqual(without["files"][f"{name}.f64"], with_h["files"][f"{name}.f64"], name)
                ea = next(e for e in without["fields"] if e["name"] == name)
                eb = next(e for e in with_h["fields"] if e["name"] == name)
                self.assertEqual(ea, eb, name)
            for key, value in without["source_checks"].items():
                self.assertEqual(with_h["source_checks"][key], value, key)
            self.assertEqual(set(with_h["source_checks"]) - set(without["source_checks"]), {"holdings"})
            self.assertEqual({k: v for k, v in without.items() if k not in ("fields", "files", "source_checks")},
                             {k: v for k, v in with_h.items() if k not in ("fields", "files", "source_checks")})
            # holdings inputs named but no holdings field requested: the builder's output is unchanged
            plain = fx.run("plain", fields=recipe, **kw)
            self.assertEqual(plain["files"], without["files"])
        self.assertLessEqual(len(recipe) + len(ALL), 64)  # runner field limit
        for x in ALL:
            self.assertNotIn(x, tool.ALL_FIELDS)


if __name__ == "__main__":
    unittest.main()
