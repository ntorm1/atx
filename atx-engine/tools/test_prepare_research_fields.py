"""Small synthetic postimplementation checks; no real archive/warehouse/lake access."""
import contextlib
import datetime as dt
import hashlib
import io
import json
import math
from pathlib import Path
import tempfile
import unittest

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import prepare_research_fields as tool

DAY_NS = 86_400_000_000_000
IDS = [101, 202, 303, 404, 505]
SESSIONS = [d for d in (dt.date(2024, 10, 1) + dt.timedelta(days=i) for i in range(92)) if d.weekday() < 5]
FORMATIONS = [dt.date(2024, 8, 30), dt.date(2024, 9, 30), dt.date(2024, 10, 31), dt.date(2024, 11, 29), dt.date(2024, 12, 31)]
SPLIT_303 = dt.date(2024, 11, 1)
C81_303 = dt.date(2024, 12, 2)  # an above-ceiling row inside the role: 303 withheld from here on only
SHARES_202_CHANGE = dt.date(2024, 8, 15)
LAST_SHARES_505 = dt.date(2023, 6, 15)
DUP = (10, 202)
# Declared IV domain [0.02, 5.0]: (t, sid) -> atmCenI_21d; 63d = v + 0.05 and 126d = v + 0.1 in float32.
IV_OVERRIDES = {(13, 303): 1e16,           # garbage: above in every tenor
                (14, 202): 0.0199,         # 21d below; 63d/126d in domain
                (15, 202): 0.02,           # float32(0.02) is exactly on the lower bound: kept
                (16, 101): 5.0,            # 21d on the upper bound: kept; 63d 5.05 / 126d 5.1 above
                (17, 101): float("inf"),   # +inf: above in every tenor
                (18, 404): -0.3,           # negative: below in every tenor
                (60, 101): 9.0}            # above in every tenor, on a NON-member cell (member[60, 101] == 0)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def day(d):
    return (d - dt.date(1970, 1, 1)).days


def iv21(i, t):
    return np.float32(round(0.2 + 0.01 * i + 0.0001 * t, 4))


def write_tickerhistory(path):
    rows = {k: [] for k in ("tradingDate", "securityID", "close", "volume", "shares", "earnFlag", "cumulReturnFactor",
                            "atmCenI_21d", "atmCenI_63d", "atmCenI_126d")}
    session_index = {d: t for t, d in enumerate(SESSIONS)}
    start, stop = dt.date(2023, 4, 3), dt.date(2025, 1, 31)  # includes 2025 rows that must be skipped
    days = [start + dt.timedelta(days=i) for i in range((stop - start).days + 1)]
    for d in (x for x in days if x.weekday() < 5):
        t = session_index.get(d, -1)
        for i, sid in enumerate(IDS + [999]):
            shares, crf = 1000, 1.0
            if sid == 202:
                shares = 3000 if d >= SHARES_202_CHANGE else 2000
            if sid == 303:
                shares, crf = (1000, 1.0) if d >= SPLIT_303 else (500, 0.5)
                if d == C81_303:
                    shares = 150_000_000
            if sid == 404 and d == dt.date(2024, 3, 1):
                shares = 200_000_000  # above the A9 thousands ceiling: whole line withheld (C-81)
            if sid == 505:
                shares = 700 if d <= LAST_SHARES_505 else None
            flag = "N"
            if sid == 101 and t in (5, 6, 7):
                flag = {5: "-1", 6: "0", 7: "1"}[t]
            if sid == 202 and t == 8:
                flag = None
            if sid == 303 and t == 9:
                flag = "X"
            v = iv21(i, max(t, 0))
            if sid == 505:
                v = None
            if sid == 303 and t == 12:
                v = np.float32(0.0)
            if (t, sid) in IV_OVERRIDES:
                v = np.float32(IV_OVERRIDES[(t, sid)])
            for k, x in (("tradingDate", d), ("securityID", sid), ("close", 10.0), ("volume", 1e6), ("shares", shares),
                         ("earnFlag", flag), ("cumulReturnFactor", crf), ("atmCenI_21d", v),
                         ("atmCenI_63d", None if v is None else np.float32(v + np.float32(0.05))),
                         ("atmCenI_126d", None if v is None else np.float32(v + np.float32(0.1)))):
                rows[k].append(x)
    # One positive duplicate key (all its rows quarantined) and one weekend row (off calendar).
    t, sid = DUP
    extra = [(SESSIONS[t], sid, 0.9), (dt.date(2024, 10, 5), 101, 0.7)]
    for d, sid, v in extra:
        for k, x in (("tradingDate", d), ("securityID", sid), ("close", 10.0), ("volume", 1e6), ("shares", 2000),
                     ("earnFlag", "N"), ("cumulReturnFactor", 1.0), ("atmCenI_21d", np.float32(v)),
                     ("atmCenI_63d", np.float32(v)), ("atmCenI_126d", np.float32(v))):
            rows[k].append(x)
    order = np.random.default_rng(7).permutation(len(rows["securityID"]))  # dates mixed in every group
    table = pa.table({
        "tradingDate": pa.array([rows["tradingDate"][i] for i in order], pa.date32()),
        "securityID": pa.array([rows["securityID"][i] for i in order], pa.int64()),
        "close": pa.array([rows["close"][i] for i in order], pa.float32()),
        "volume": pa.array([rows["volume"][i] for i in order], pa.float64()),
        "shares": pa.array([rows["shares"][i] for i in order], pa.int64()),
        "earnFlag": pa.array([rows["earnFlag"][i] for i in order], pa.string()),
        "cumulReturnFactor": pa.array([rows["cumulReturnFactor"][i] for i in order], pa.float64()),
        **{k: pa.array([rows[k][i] for i in order], pa.float32()) for k in ("atmCenI_21d", "atmCenI_63d", "atmCenI_126d")}})
    pq.write_table(table, path, row_group_size=700)


FINRA_SHARES = [  # (security_id, available_at, value)
    (101, "2024-09-25", "100"), (101, "2024-10-09", "110"), (101, "2024-10-21", "120"), (101, "2024-12-24", "130"),
    (202, "2024-10-01", "50"), (202, "2024-10-15", "nan"),
    (303, "2024-10-02", "7.5"),
    (999, "2024-10-02", "1"),
    (101, "2025-01-10", "999"),
]
FINRA_DTC = [(101, "2024-09-25", "1.5"), (202, "2024-10-01", "2.25")]


def write_finra(root, shares_rows=FINRA_SHARES, dtc_rows=FINRA_DTC):
    (root / "asof").mkdir(parents=True)
    dates = sorted({r[1] for r in shares_rows + dtc_rows} | {"2021-06-09"})
    lines = ["settlement_date,due_date,dissemination_date,source,third_column_label,in_download,source_url"]
    lines.append("2021-05-28,2021-06-02,2021-06-09,official,Publication Date,True,x")
    lines += [f"{(dt.date.fromisoformat(d) - dt.timedelta(days=9)).isoformat()},x,{d},official,Publication Date,True,x"
              for d in dates if d != "2021-06-09"]
    (root / "dissemination_schedule.csv").write_text("\n".join(lines) + "\n", encoding="utf-8")
    outputs = {}
    for name, rows in (("si_shares", shares_rows), ("si_dtc", dtc_rows)):
        path = root / "asof" / f"{name}.csv"
        path.write_bytes(("security_id,available_at,value\n" + "".join(f"{a},{b},{c}\n" for a, b, c in rows)).encode())
        outputs[name] = {"sha256": sha(path)}
    (root / "asof" / "manifest.json").write_text(json.dumps({"outputs": outputs}), encoding="utf-8")


def write_lake(root, formations=FORMATIONS):
    rows = {"eom": [], "line_id": [], "price": [], "me_line": [], "size_grp": [], "formation_date": []}
    groups = ["micro", "small", "large", "mega", "small"]
    for k, f in enumerate(formations):
        eom = (f.replace(day=28) + dt.timedelta(days=4)).replace(day=1) - dt.timedelta(days=1)
        for sid, me, sg in ((101, 1e9 * (k + 1), groups[k]), (202, 5e8, None if k == 2 else "large"),
                            (303, 2e8 + k, "micro"), (999, 1e7, "micro")):
            if sid == 303 and k == 2:
                continue
            for c, v in (("eom", eom), ("line_id", f"TBLTICKERHISTORY-{sid}"), ("price", 1.0), ("me_line", me),
                         ("size_grp", sg), ("formation_date", f)):
                rows[c].append(v)
    spine = root / "spine_monthly" / "year=2024" / "part-0.parquet"
    spine.parent.mkdir(parents=True)
    pq.write_table(pa.table({"eom": pa.array(rows["eom"], pa.date32()), "line_id": rows["line_id"],
                             "price": rows["price"], "me_line": pa.array(rows["me_line"], pa.float64()),
                             "size_grp": pa.array(rows["size_grp"], pa.string()),
                             "formation_date": pa.array(rows["formation_date"], pa.date32())}), spine)
    types = root / "line_types" / "year=0" / "part-0.parquet"
    types.parent.mkdir(parents=True)
    pq.write_table(pa.table({"line_id": [f"TBLTICKERHISTORY-{s}" for s in (101, 202, 303, 505, 999)],
                             "security_type": ["common", "common_unverified", "ETF", "unknown", "ETF"]}), types)
    entry = lambda p: {"path": p.relative_to(root).as_posix(), "bytes": p.stat().st_size, "sha256": sha(p)}
    (root / "_manifest.json").write_text(json.dumps({
        "snapshot_id": "fixture", "snapshot_sha256": "0" * 64,
        "datasets": {"spine_monthly": {"files": [entry(spine)]}, "line_types": {"files": [entry(types)]}}}), encoding="utf-8")


def role_prices(nd):
    """Adjusted/raw closes and presence with every mkt_ret exclusion path exercised."""
    t = np.arange(nd)[:, None]
    k = np.arange(1, len(IDS) + 1)[None, :]
    close = 10.0 * k * np.exp(0.001 * k * t + 0.002 * np.sin(t + k))
    raw = close.copy()
    close[20:, 2] *= np.exp(2.0)
    raw[20:, 2] *= np.exp(2.0)           # |log adj| > 1.5 even though raw agrees: guarded
    close[30:, 3] *= np.exp(0.2)         # adjusted jumps, raw does not: |adj| > |raw| + 0.10: guarded
    raw[50, 1] = np.nan                  # present but unpriced raw endpoint
    close[55, 0] = 0.0                   # present but non-positive adjusted endpoint
    present = np.ones((nd, len(IDS)), dtype="u1")
    present[40, 4] = 0                   # absent: no return on either side, NaN broadcast cell
    close[40, 4] = np.nan
    raw[40, 4] = np.nan
    return close, raw, present


def expected_mkt_ret(close, raw, present, member):
    import math
    nd, n = close.shape
    out = np.full((nd, n), np.nan)
    for d in range(1, nd):
        rs = []
        for i in range(n):
            a, b = (close[d - 1, i], raw[d - 1, i]), (close[d, i], raw[d, i])
            if not (member[d - 1, i] and present[d - 1, i] and present[d, i]):
                continue
            if not all(math.isfinite(x) and x > 0 for x in (*a, *b)):
                continue
            r = b[0] / a[0] - 1
            lr, rr = math.log(b[0]) - math.log(a[0]), math.log(b[1]) - math.log(a[1])
            if not math.isfinite(r) or abs(lr) > 1.5 or abs(lr) > abs(rr) + 0.10:
                continue
            rs.append(r)
        if rs:
            out[d, present[d] != 0] = math.fsum(rs) / len(rs)
    return out


def write_role(root, source_sha, sessions=SESSIONS):
    root.mkdir(parents=True)
    member = np.ones((len(sessions), len(IDS)), dtype="u1")
    member[0:3, 4] = 0
    member[60, 0] = 0
    close, raw, present = role_prices(len(sessions))
    blobs = {"sessions.i64": np.array([day(d) * DAY_NS for d in sessions], dtype="<i8").tobytes(),
             "ids.u64": np.array(IDS, dtype="<u8").tobytes(), "member.u8": member.tobytes(),
             "close.f64": close.astype("<f8").tobytes(), "raw_close.f64": raw.astype("<f8").tobytes(),
             "present.u8": present.tobytes(),
             "volume.f64": np.where(present != 0, 1e4, np.nan).astype("<f8").tobytes()}  # ~1%/day of shares_out
    files = {}
    for name, blob in blobs.items():
        (root / name).write_bytes(blob)
        files[name] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete", "dates": len(sessions),
                "instruments": len(IDS), "instrument_namespace": "spiderrock.securityID", "score_begin": 10,
                "score_end": len(sessions), "source_sha256": source_sha, "files": files,
                "clock_recipe": "modeled-session+22h-mark+23h-decision-v1"}
    (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return sha(root / "manifest.json")


class Fixture:
    def __init__(self, base: Path, formations=FORMATIONS):
        self.base = base
        self.th = base / "TickerHistory3.parquet"
        write_tickerhistory(self.th)
        self.finra = base / "finra"
        write_finra(self.finra)
        self.lake = base / "lake"
        write_lake(self.lake, formations)
        self.role = base / "role"
        self.role_sha = write_role(self.role, sha(self.th))

    def run(self, out, **kw):
        kw.setdefault("finra", self.finra)
        kw.setdefault("tickerhistory", self.th)
        kw.setdefault("lake", self.lake)
        with contextlib.redirect_stdout(io.StringIO()):
            return tool.run(self.role, kw.pop("role_sha", self.role_sha), self.base / out, **kw)

    def field(self, out, name):
        return np.fromfile(self.base / out / f"{name}.f64", dtype="<f8").reshape(len(SESSIONS), len(IDS))


def t_of(date_text):
    return SESSIONS.index(dt.date.fromisoformat(date_text))


class ResearchFields(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.fx = Fixture(Path(cls.temp.name))
        cls.manifest = cls.fx.run("fields", fields=list(tool.FIELDS))  # every field, opt-in ones included

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_finra_strict_available_at_staleness_and_visible_nan(self):
        si = self.fx.field("fields", "si_shares")
        a = IDS.index(101)
        self.assertEqual(si[t_of("2024-10-01"), a], 100)
        self.assertEqual(si[t_of("2024-10-09"), a], 100)   # published that day: not visible yet
        self.assertEqual(si[t_of("2024-10-10"), a], 110)   # first session after availability
        self.assertEqual(si[t_of("2024-10-21"), a], 110)
        self.assertEqual(si[t_of("2024-10-22"), a], 120)
        self.assertEqual(si[t_of("2024-12-05"), a], 120)   # age 45: still visible
        self.assertTrue(np.isnan(si[t_of("2024-12-06"), a]))  # age 46: stale
        self.assertTrue(np.isnan(si[t_of("2024-12-24"), a]))
        self.assertEqual(si[t_of("2024-12-26"), a], 130)
        b = IDS.index(202)
        self.assertTrue(np.isnan(si[t_of("2024-10-01"), b]))  # nothing strictly before
        self.assertEqual(si[t_of("2024-10-15"), b], 50)
        self.assertTrue(np.isnan(si[t_of("2024-10-16"), b]))  # visible NaN wins, no skip-back
        self.assertTrue(np.all(np.isnan(si[:, IDS.index(404)])))
        dtc = self.fx.field("fields", "si_dtc")
        self.assertEqual(dtc[0, a], 1.5)
        self.assertEqual(dtc[t_of("2024-11-15"), b], 2.25)  # 2024-10-01 + 45
        self.assertTrue(np.isnan(dtc[t_of("2024-11-18"), b]))
        checks = self.manifest["source_checks"]["si_shares"]
        self.assertEqual(checks["rows_available_on_or_after_2025_dropped"], 1)
        self.assertEqual(checks["rows_ignored_unknown_id"], 1)

    def test_tickerhistory_same_date_alignment_duplicates_and_earn(self):
        iv = self.fx.field("fields", "iv_atm_21d")
        for t in (0, 7, 30, len(SESSIONS) - 1):
            for i in range(3):
                if (t, IDS[i]) != DUP:
                    self.assertEqual(iv[t, i], float(iv21(i, t)))
        self.assertTrue(np.isnan(iv[DUP[0], IDS.index(DUP[1])]))      # duplicate key quarantined
        self.assertTrue(np.isnan(iv[12, IDS.index(303)]))              # non-positive IV
        self.assertTrue(np.all(np.isnan(iv[:, IDS.index(505)])))       # null IV
        iv126 = self.fx.field("fields", "iv_atm_126d")
        self.assertEqual(iv126[3, 0], float(np.float32(iv21(0, 3) + np.float32(0.1))))
        earn = self.fx.field("fields", "earn_recent")
        a = IDS.index(101)
        self.assertEqual(list(earn[4:9, a]), [0, 0, 1, 1, 0])  # N, -1 -> 0; 0, 1 -> 1
        self.assertTrue(np.isnan(earn[8, IDS.index(202)]))     # null flag
        self.assertTrue(np.isnan(earn[9, IDS.index(303)]))     # unexpected flag
        self.assertTrue(np.isnan(earn[DUP[0], IDS.index(DUP[1])]))
        self.assertEqual(earn[t_of("2024-10-07"), a], 0)       # weekend row 10-05 never lands anywhere
        checks = self.manifest["source_checks"]["tickerhistory"]
        self.assertEqual(checks["duplicate_keys_quarantined"], 1)
        self.assertEqual(checks["rows_off_role_calendar"], 1)
        self.assertEqual(checks["earnflag_unexpected"], 1)
        self.assertGreater(checks["rows_on_or_after_2025_skipped"], 0)
        self.assertEqual(checks["iv_nonpositive_or_nonfinite"], 7)  # row level, all tenors: 0.0, 3x inf, 3x -0.3

    def test_iv_declared_domain_to_nan_and_counted(self):
        tenors = {"iv_atm_21d": 0.0, "iv_atm_63d": 0.05, "iv_atm_126d": 0.1}
        entries = {f["name"]: f for f in self.manifest["fields"]}
        expected_counts = {"iv_atm_21d": (3, 3), "iv_atm_63d": (1, 4), "iv_atm_126d": (1, 4)}
        for name, add in tenors.items():
            got = self.fx.field("fields", name)
            for (t, sid), v21 in IV_OVERRIDES.items():
                v = np.float32(np.float32(v21) + np.float32(add)) if add else np.float32(v21)
                keep = bool(np.float32(0.02) <= v <= np.float32(5.0))
                cell = got[t, IDS.index(sid)]
                if keep:
                    self.assertEqual(cell, float(v), (name, t, sid))  # never clamped or rescaled
                else:
                    self.assertTrue(np.isnan(cell), (name, t, sid))
            if name == "iv_atm_21d":
                self.assertTrue(np.isnan(got[12, IDS.index(303)]))            # 0.0: below
                self.assertEqual(got[15, IDS.index(202)], float(np.float32(0.02)))  # inclusive lower bound
                self.assertEqual(got[16, IDS.index(101)], 5.0)                # inclusive upper bound
            else:
                self.assertEqual(got[12, IDS.index(303)], float(np.float32(add)))  # 0.05 / 0.1: in domain
            plaus = entries[name]["plausibility"]
            below, above = expected_counts[name]
            self.assertEqual((plaus["min"], plaus["max"], plaus["inclusive"]), (0.02, 5.0, True))
            self.assertEqual((plaus["below_min"], plaus["above_max"]), (below, above), name)
            self.assertEqual(plaus["implausible_to_nan"], below + above)
            self.assertEqual(plaus["implausible_to_nan_member"], below + above - 1)  # (60, 101) is not a member
            self.assertEqual((plaus["member_below_min"], plaus["member_above_max"]), (below, above - 1))
            cov = entries[name]["coverage"]
            self.assertGreaterEqual(cov["member_finite_min"], float(np.float32(0.02)))
            self.assertLessEqual(cov["member_finite_max"], 5.0)
            q = cov["member_finite_quantiles"]
            self.assertEqual(list(q), ["p0.1", "p1", "p50", "p99", "p99.9"])
            self.assertTrue(cov["member_finite_min"] <= q["p0.1"] <= q["p50"] <= q["p99.9"] <= cov["member_finite_max"])
        for name, entry in entries.items():
            self.assertEqual("plausibility" in entry, name in tenors or name == "shares_out")
        plaus = entries["shares_out"]["plausibility"]  # every main-fixture count is inside [1e5, 5e10]
        self.assertEqual((plaus["min"], plaus["max"], plaus["implausible_to_nan"]), (1e5, 5e10, 0))
        fb = entries["shares_out"]["factor_break"]  # five names cannot make a mass session
        self.assertEqual((fb["mass_sessions"], fb["role_repair_mass_sessions"], fb["restated_cells"]), ([], [], 0))
        # 303's 2:1 factor step on a flat raw close is one jump cell: the margin statistic.
        self.assertEqual((fb["max_non_mass_jump_cells"], fb["max_non_mass_session"]), (1, SPLIT_303.isoformat()))

    def test_shares_out_lag90_restated_and_withheld(self):
        so = self.fx.field("fields", "shares_out")
        self.assertTrue(np.all(so[:, 0] == 1_000_000))
        b = IDS.index(202)
        switch = SHARES_202_CHANGE + dt.timedelta(days=90)
        for t, d in enumerate(SESSIONS):
            if t == DUP[0]:
                self.assertTrue(np.isnan(so[t, b]))  # no clean same-date factor row
            else:
                self.assertEqual(so[t, b], 3_000_000 if d >= switch else 2_000_000, d)
        c = IDS.index(303)
        self.assertEqual(so[t_of("2024-10-15"), c], 500_000)   # pre-split basis
        self.assertEqual(so[t_of("2024-11-04"), c], 1_000_000)  # lag row pre-split, restated 2:1
        self.assertEqual(so[t_of("2024-11-29"), c], 1_000_000)  # before its above-ceiling row: not withheld
        for t, d in enumerate(SESSIONS):  # C-81 point in time: withheld from the offending row's date on
            self.assertEqual(np.isnan(so[t, c]), d >= C81_303, d)
        self.assertTrue(np.all(np.isnan(so[:, IDS.index(404)])))  # A9 row before the role -> withheld throughout
        e = IDS.index(505)
        edge = LAST_SHARES_505 + dt.timedelta(days=490)
        for t, d in enumerate(SESSIONS):
            self.assertEqual(np.isnan(so[t, e]), d > edge, d)
        checks = self.manifest["source_checks"]["tickerhistory"]
        self.assertEqual(checks["shares_lines_withheld_c81"], 2)
        self.assertEqual(checks["shares_cells_withheld_c81"], len(SESSIONS) + sum(d >= C81_303 for d in SESSIONS))

    def test_spine_strict_formation_and_static_type(self):
        me = self.fx.field("fields", "mktcap_lagged")
        sg = self.fx.field("fields", "size_grp")
        a, b, c = IDS.index(101), IDS.index(202), IDS.index(303)
        self.assertEqual(me[t_of("2024-10-01"), a], 2e9)   # Sep formation
        self.assertEqual(me[t_of("2024-10-31"), a], 2e9)   # formation session itself: still Sep (strict)
        self.assertEqual(me[t_of("2024-11-01"), a], 3e9)   # Oct formation
        self.assertEqual(sg[t_of("2024-11-01"), a], 2.0)   # large
        self.assertEqual(sg[t_of("2024-12-02"), a], 3.0)   # mega (Nov formation)
        self.assertTrue(np.isnan(sg[t_of("2024-11-01"), b]))   # NULL size_grp
        self.assertEqual(me[t_of("2024-11-01"), b], 5e8)
        self.assertTrue(np.isnan(me[t_of("2024-11-01"), c]))   # absent at the Oct formation
        self.assertEqual(me[t_of("2024-12-02"), c], 2e8 + 3)
        self.assertTrue(np.all(np.isnan(me[:, IDS.index(404)])))
        self.assertEqual(me[t_of("2024-12-31"), a], 4e9)       # Dec formation (2024-12-31) never used
        ic = self.fx.field("fields", "is_common")
        self.assertEqual(list(ic[0, :3]), [1.0, 1.0, 0.0])
        self.assertTrue(np.isnan(ic[0, IDS.index(404)]))
        self.assertEqual(ic[0, IDS.index(505)], 0.0)
        self.assertTrue(np.array_equal(ic, np.broadcast_to(ic[0], ic.shape), equal_nan=True))  # static
        self.assertEqual(self.manifest["source_checks"]["lake"]["spine_max_formation_age_days"], 32)  # Dec 31 <- Nov 29

    def test_stale_spine_formation_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            fx = Fixture(Path(temp), formations=FORMATIONS[:3])  # lake ends with the October formation
            with self.assertRaisesRegex(ValueError, r"stale spine formation: session 2024-12-06 .* 2024-10-31 \(36 days"):
                fx.run("stale", fields=["size_grp"])
            self.assertFalse((fx.base / "stale" / "manifest.json").exists())

    def test_point_in_time_flags_and_default_fields(self):
        opt_in = {"is_common", "mktcap_lagged", "size_grp"}
        self.assertEqual(set(tool.FIELDS) - set(tool.DEFAULT_FIELDS), opt_in)
        m = self.manifest
        self.assertEqual(m["non_point_in_time_fields"], ["mktcap_lagged", "size_grp", "is_common"])
        self.assertIn("point_in_time", m["point_in_time_definition"])
        for e in m["fields"]:
            self.assertIs(e["point_in_time"], e["name"] not in opt_in, e["name"])
            if e["point_in_time"]:
                self.assertEqual(e["non_pit_aspects"], [])
                self.assertNotIn("point_in_time_reason", e)
            else:
                self.assertTrue(e["point_in_time_reason"])
                self.assertEqual(e["non_pit_aspects"], ["values"] if e["name"] == "is_common" else ["presence"])
        # The default run publishes the point-in-time fields only, byte-identical to the full run.
        d = self.fx.run("fields-default")
        self.assertEqual([e["name"] for e in d["fields"]], list(tool.DEFAULT_FIELDS))
        self.assertEqual(d["non_point_in_time_fields"], [])
        self.assertTrue(all(e["point_in_time"] for e in d["fields"]))
        self.assertEqual(sorted(p.name for p in (self.fx.base / "fields-default").iterdir()),
                         sorted([f"{n}.f64" for n in tool.DEFAULT_FIELDS] + ["manifest.json"]))
        for name in tool.DEFAULT_FIELDS:
            self.assertEqual(d["files"][f"{name}.f64"], m["files"][f"{name}.f64"], name)

    def test_finra_vintage_structured(self):
        for name in ("si_shares", "si_dtc"):
            e = next(f for f in self.manifest["fields"] if f["name"] == name)
            self.assertEqual(e["vintage_safe_from"], "2021-06-10")  # fixture schedule: 2021-06-09 cutoff
            v = e["coverage"]["vintage_risk"]
            self.assertEqual((v["finite_member_cells"], v["last_session_with_republished_visible_cell"]), (0, None))
            self.assertEqual(v["first_session_vintage_safe"], SESSIONS[0].isoformat())
        # A cutoff inside the role (2024-10-09): rows available on or before it are "republished".
        with tempfile.TemporaryDirectory() as temp:
            role = tool.Role(self.fx.role, self.fx.role_sha)
            dissemination, _, source = tool.read_schedule(self.fx.finra)
            with contextlib.redirect_stdout(io.StringIO()):
                w, _, cov, _, extra = tool.finra_field("si_shares", self.fx.finra, role, Path(temp),
                                                       (dissemination, day(dt.date(2024, 10, 9)), source), tool.Budget())
            self.assertEqual(extra["vintage_safe_from"], "2024-10-10")
            v = cov["vintage_risk"]
            # 303's 2024-10-02 row stays visible through age 45 (2024-11-16, a Saturday).
            self.assertEqual(v["last_session_with_republished_visible_cell"], "2024-11-15")
            self.assertEqual(v["first_session_vintage_safe"], "2024-11-18")
            si = np.fromfile(Path(temp) / "si_shares.f64", dtype="<f8").reshape(len(SESSIONS), len(IDS))
            member = np.fromfile(self.fx.role / "member.u8", dtype="u1").reshape(len(SESSIONS), len(IDS)) != 0
            # 101: rows 09-25 and 10-09 visible through 10-21; 202: 10-01 row through 10-15; 303: through 11-15.
            expect = 0
            for t, d in enumerate(SESSIONS):
                for i, last in ((0, dt.date(2024, 10, 21)), (1, dt.date(2024, 10, 15)), (2, dt.date(2024, 11, 15))):
                    expect += int(d <= last and member[t, i] and np.isfinite(si[t, i]))
            self.assertEqual(v["finite_member_cells"], expect)
            self.assertGreater(expect, 0)

    def test_mkt_ret_guarded_equal_weight_broadcast(self):
        close, raw, present = role_prices(len(SESSIONS))
        member = np.fromfile(self.fx.role / "member.u8", dtype="u1").reshape(len(SESSIONS), len(IDS))
        got = self.fx.field("fields", "mkt_ret")
        np.testing.assert_array_equal(got, expected_mkt_ret(close, raw, present, member))
        self.assertTrue(np.all(np.isnan(got[0])))
        for d in range(1, len(SESSIONS)):
            live = present[d] != 0
            self.assertEqual(len(set(got[d, live].tolist())), 1)       # one scalar per session
            self.assertTrue(np.all(np.isnan(got[d, ~live])))
        self.assertTrue(np.isnan(got[40, 4]))
        r = close[20] / close[19] - 1
        self.assertAlmostEqual(got[20, 0], float(np.mean(r[[0, 1, 3, 4]])), places=15)  # 303 guarded (|log| > 1.5)
        r = close[30] / close[29] - 1
        self.assertAlmostEqual(got[30, 0], float(np.mean(r[[0, 1, 2, 4]])), places=15)  # 404 guarded (adj vs raw)
        r = close[61] / close[60] - 1
        self.assertAlmostEqual(got[61, 1], float(np.mean(r[[1, 2, 3, 4]])), places=15)  # 101 not a member at 60
        entry = next(f for f in self.manifest["fields"] if f["name"] == "mkt_ret")
        stats = entry["stats"]
        self.assertEqual(stats["contributors_per_session"]["min"], 4)
        self.assertEqual(stats["contributors_per_session"]["median"], 5.0)
        self.assertEqual(stats["contributors_per_session"]["sessions"], len(SESSIONS) - 1)
        self.assertEqual(stats["contributors_per_session"]["zero_contributor_sessions"], 0)
        self.assertEqual(stats["guarded_member_intervals"], 2)
        role = json.loads((self.fx.role / "manifest.json").read_bytes())
        self.assertEqual({Path(s["path"]).name: s["sha256"] for s in entry["sources"]},
                         {k: role["files"][k]["sha256"] for k in ("close.f64", "raw_close.f64", "present.u8", "member.u8")})
        self.assertEqual(entry["sha256"], sha(self.fx.base / "fields" / "mkt_ret.f64"))
        self.assertIn("definition", entry)

    def test_manifest_hashes_axes_and_determinism(self):
        m = self.manifest
        role = json.loads((self.fx.role / "manifest.json").read_bytes())
        self.assertEqual(m["schema"], "atx.research-role-fields/v1")
        self.assertEqual(m["role"]["manifest_sha256"], self.fx.role_sha)
        self.assertEqual(m["role"]["sessions_sha256"], role["files"]["sessions.i64"]["sha256"])
        self.assertEqual(m["role"]["ids_sha256"], role["files"]["ids.u64"]["sha256"])
        self.assertEqual([f["name"] for f in m["fields"]], list(tool.FIELDS))
        for name, entry in m["files"].items():
            path = self.fx.base / "fields" / name
            self.assertEqual(entry["bytes"], len(SESSIONS) * len(IDS) * 8)
            self.assertEqual(entry["sha256"], sha(path))
        th = next(f for f in m["fields"] if f["name"] == "iv_atm_21d")
        self.assertEqual(th["sources"][0]["sha256"], sha(self.fx.th))
        cov = next(f for f in m["fields"] if f["name"] == "shares_out")["coverage"]
        self.assertEqual(cov["member_cells"], len(SESSIONS) * len(IDS) - 4)
        member = np.fromfile(self.fx.role / "member.u8", dtype="u1").reshape(len(SESSIONS), len(IDS)) != 0
        for e in m["fields"]:  # quantiles are of exactly the finite member cells of the published bytes
            got = self.fx.field("fields", e["name"])
            v = got[member & np.isfinite(got)]
            q = e["coverage"]["member_finite_quantiles"]
            self.assertEqual(list(q.values()), np.quantile(v, [0.001, 0.01, 0.5, 0.99, 0.999]).tolist(), e["name"])
        again = self.fx.run("fields-again", fields=list(tool.FIELDS))
        self.assertEqual(again, m)
        for name in list(m["files"]) + ["manifest.json"]:
            self.assertEqual(sha(self.fx.base / "fields" / name), sha(self.fx.base / "fields-again" / name))

    def test_code_identity_is_line_ending_independent(self):
        m = self.manifest
        lf = Path(tool.__file__).read_bytes().replace(b"\r\n", b"\n")
        crlf = self.fx.base / "crlf_copy.py"
        crlf.write_bytes(lf.replace(b"\n", b"\r\n"))
        a, b = tool.code_identity(Path(tool.__file__)), tool.code_identity(crlf)
        self.assertEqual(a, {k: m[k] for k in ("code_sha256", "code_sha256_lf", "code_git_blob_sha1")})
        self.assertEqual(b["code_sha256"], sha(crlf))                  # the raw pin follows the checkout bytes
        self.assertNotEqual(b["code_sha256"], b["code_sha256_lf"])
        self.assertEqual((a["code_sha256_lf"], a["code_git_blob_sha1"]), (b["code_sha256_lf"], b["code_git_blob_sha1"]))
        self.assertEqual(a["code_git_blob_sha1"], hashlib.sha1(b"blob %d\0" % len(lf) + lf).hexdigest())

    def test_exclusive_output_and_fail_closed_refusals(self):
        before = sha(self.fx.base / "fields" / "manifest.json")
        with self.assertRaises(FileExistsError):
            self.fx.run("fields", fields=["si_dtc"])
        self.assertEqual(sha(self.fx.base / "fields" / "manifest.json"), before)
        (self.fx.base / "empty-existing").mkdir()
        with self.assertRaises(FileExistsError):  # even an empty pre-existing directory is refused
            self.fx.run("empty-existing", fields=["is_common"])
        self.assertEqual(list((self.fx.base / "empty-existing").iterdir()), [])
        with self.assertRaisesRegex(ValueError, "--role-sha256"):
            self.fx.run("bad-sha", role_sha="0" * 64)
        with self.assertRaisesRegex(ValueError, "--fields"):
            self.fx.run("bad-field", fields=["si_shares", "hv_21d"])
        # A role reaching 2025 is refused before any output is created.
        sealed = self.fx.base / "role-2025"
        sealed_sha = write_role(sealed, sha(self.fx.th), SESSIONS + [dt.date(2025, 1, 2)])
        with self.assertRaisesRegex(ValueError, "2025"):
            with contextlib.redirect_stdout(io.StringIO()):
                tool.run(sealed, sealed_sha, self.fx.base / "out-2025", finra=self.fx.finra,
                         tickerhistory=self.fx.th, lake=self.fx.lake)
        self.assertFalse((self.fx.base / "out-2025").exists())
        # A different vendor file than the role's source: refused, nothing published.
        other = self.fx.base / "role-other"
        other_sha = write_role(other, "f" * 64)
        with self.assertRaisesRegex(ValueError, "source_sha256"):
            with contextlib.redirect_stdout(io.StringIO()):
                tool.run(other, other_sha, self.fx.base / "out-other", ["iv_atm_21d"], finra=self.fx.finra,
                         tickerhistory=self.fx.th, lake=self.fx.lake)
        self.assertFalse((self.fx.base / "out-other" / "manifest.json").exists())

    def test_source_pins_refuse_changed_inputs(self):
        with tempfile.TemporaryDirectory() as temp:
            fx = Fixture(Path(temp))
            spine = fx.lake / "spine_monthly" / "year=2024" / "part-0.parquet"
            spine.write_bytes(spine.read_bytes() + b"\0")
            with self.assertRaisesRegex(ValueError, "lake manifest"):
                fx.run("lake-bad", fields=["mktcap_lagged"])
            self.assertFalse((fx.base / "lake-bad" / "manifest.json").exists())
            close_path = fx.role / "close.f64"
            blob = bytearray(close_path.read_bytes())
            blob[-8:] = np.array([123.0], dtype="<f8").tobytes()
            close_path.write_bytes(bytes(blob))  # same size, different bytes: caught by the streamed hash
            with self.assertRaisesRegex(ValueError, "close.f64 bytes do not match"):
                fx.run("role-bad", fields=["mkt_ret"])
            self.assertFalse((fx.base / "role-bad" / "manifest.json").exists())
            with self.assertRaisesRegex(ValueError, "close.f64 bytes do not match"):  # the units-rule stream too
                fx.run("role-bad-shares", fields=["si_shares", "shares_out"])
            self.assertFalse((fx.base / "role-bad-shares" / "manifest.json").exists())
            csv_path = fx.finra / "asof" / "si_dtc.csv"
            csv_path.write_bytes(csv_path.read_bytes() + b"303,2024-10-02,1\n")
            with self.assertRaisesRegex(ValueError, "receipt"):
                fx.run("finra-bad", fields=["si_dtc"])
            # The strict CSV contract: duplicates and non-dissemination dates are refused.
            with self.assertRaisesRegex(ValueError, "duplicate"):
                tool.parse_asof_csv(b"security_id,available_at,value\n1,2024-01-02,1\n1,2024-01-02,2\n")
            with self.assertRaisesRegex(ValueError, "finite decimal"):
                tool.parse_asof_csv(b"security_id,available_at,value\n1,2024-01-02,inf\n")
            with self.assertRaisesRegex(ValueError, "header"):
                tool.parse_asof_csv(b"\xef\xbb\xbfsecurity_id,available_at,value\n")
            ids, days, values = tool.parse_asof_csv(b"security_id,available_at,value\r\n5,2024-01-03,NaN\r\n5,2024-01-02,\r\n")
            self.assertEqual(list(ids), [5, 5])
            self.assertEqual(list(days), [day(dt.date(2024, 1, 2)), day(dt.date(2024, 1, 3))])
            self.assertTrue(np.all(np.isnan(values)))

    def test_cli_subset(self):
        with contextlib.redirect_stdout(io.StringIO()):
            tool.main(["--role", str(self.fx.role), "--role-sha256", self.fx.role_sha,
                       "--output", str(self.fx.base / "cli"), "--fields", "is_common,si_shares",
                       "--finra", str(self.fx.finra), "--tickerhistory", str(self.fx.th), "--lake", str(self.fx.lake),
                       "--max-rss-mib", "700"])
        m = json.loads((self.fx.base / "cli" / "manifest.json").read_bytes())
        self.assertEqual([f["name"] for f in m["fields"]], ["si_shares", "is_common"])
        self.assertEqual([f["point_in_time"] for f in m["fields"]], [True, False])  # opt-in, flagged
        self.assertEqual(m["non_point_in_time_fields"], ["is_common"])
        self.assertEqual(sorted(p.name for p in (self.fx.base / "cli").iterdir()),
                         ["is_common.f64", "manifest.json", "si_shares.f64"])
        np.testing.assert_array_equal(self.fx.field("cli", "si_shares"), self.fx.field("fields", "si_shares"))


# ---------------------------------------------------------------------------------------------------------
# Unchained vendor factor across a boundary (TickerHistory3 2021-01-04, factor-break-v1) and shares_out.
# Before FB_BREAK every line's factor is anchored at the block's own end (1.0 unless a genuine action inside
# the block moved it); from FB_BREAK on it carries every later action too, so the factor steps by the product
# of the line's LATER actions while the raw close does not move.
# ---------------------------------------------------------------------------------------------------------

def weekdays(a, b):
    return [d for d in (a + dt.timedelta(days=i) for i in range((b - a).days + 1)) if d.weekday() < 5]


FB_BREAK = dt.date(2021, 1, 4)
FB_SPLIT_1054 = dt.date(2021, 3, 1)   # genuine 4:1 forward split of 1054 (its later action seen at the break)
FB_SPLIT_1060 = dt.date(2021, 3, 15)  # genuine 2:1 split of 1060 on a normal day
FB_GAP_1056 = (dt.date(2020, 12, 24), dt.date(2021, 1, 5))  # 1056 has no rows: its step spans 14 days
FB_GAP_END = dt.date(2021, 1, 6)
FB_NULL_RAW_1059 = dt.date(2021, 2, 10)  # a row with a factor but no close: not an observation
FB_NO_ROW_1058 = dt.date(2021, 2, 3)     # a repaired line without a session row: NaN, not a restated cell
FB_CONSOL_1065 = dt.date(2021, 3, 22)  # genuine 1:10 consolidation; 50%/day turnover in both share bases
FB_SPIKE_1068 = dt.date(2021, 2, 1)    # 1068 trades 5x its share count a day from here on
FB_IDS = list(range(1001, 1070))
FB_TH_DAYS = weekdays(dt.date(2019, 5, 1), dt.date(2021, 6, 30))
FB_SESSIONS = weekdays(dt.date(2020, 9, 1), dt.date(2021, 6, 30))
FB_POST_SESSIONS = weekdays(dt.date(2021, 5, 3), dt.date(2021, 6, 30))  # a validation-like role after the break
FB_EVENTS = {1054: [(FB_SPLIT_1054, 4.0)], 1055: [(FB_BREAK, 2.0)], 1057: [(FB_BREAK, 1.05)], 1060: [(FB_SPLIT_1060, 2.0)],
             1065: [(FB_CONSOL_1065, 0.1)]}
# si_shares rows (available_at = dissemination date): 1067 at 5.5x then exactly 5.0x its 1e6 shares; 1066 (already
# caught by turnover) at 6.7x; 1059 at 1%; 1053 at 6x.
FB_FINRA = [(1059, "2021-02-01", "10000"), (1066, "2021-02-01", "1000000"),
            (1067, "2021-02-01", "5500000"), (1067, "2021-03-01", "5000000"),
            (1053, "2021-02-01", "6000000")]  # 1053: rule (b) inside its factor-break restated window
FB_REPAIRED_IDS = set(range(1001, 1053)) | {1053, 1054, 1058, 1063, 1064}
FB_HOLE_1064 = (dt.date(2020, 12, 31), dt.date(2021, 1, 5))  # 7-day step ending 2021-01-06: repaired


def fb_line(sid, d):
    """(raw close or None, cumulReturnFactor, vendor shares in thousands or None), or None for no row."""
    old = d < FB_BREAK
    if sid <= 1052:                      # dividend payer: later dividends lower the new block by 3%
        return 20.0, 1.0 if old else 0.97, 1000
    if sid == 1053:                      # later 1:10 consolidation: factor x10, raw flat
        return 20.0, 1.0 if old else 10.0, 1000
    if sid == 1054:                      # later 4:1 split: factor x1/4 at the break, genuine split on FB_SPLIT_1054
        split = d >= FB_SPLIT_1054
        return (10.0 if split else 40.0), (1.0 if old or split else 0.25), (4000 if split else 1000)
    if sid == 1055:                      # genuine 2:1 split ON the break, shown by the factor, raw followed
        return (40.0 if old else 20.0), (0.5 if old else 1.0), (1000 if old else 2000)
    if sid == 1056:                      # dividend payer with a 14-day hole across the break: kept_gap
        if FB_GAP_1056[0] <= d <= FB_GAP_1056[1]:
            return None
        return 20.0, 1.0 if old else 0.97, 1000
    if sid == 1057:                      # same-day 5% distribution on the break: kept_distribution
        return 20.0, 1.0 if old else 1.05, 1000
    if sid == 1058:                      # dividend payer whose raw rose 3% on the break: not a jump cell, repaired
        if d == FB_NO_ROW_1058:
            return None
        return (20.0 if old else 20.6), (1.0 if old else 0.97), 1000
    if sid == 1059:                      # control
        return (None if d == FB_NULL_RAW_1059 else 20.0), 1.0, 1000
    if sid == 1060:
        split = d >= FB_SPLIT_1060
        return (20.0 if split else 40.0), (1.0 if split else 0.5), (2000 if split else 1000)
    if sid == 1061:                      # 5e4 shares: below the declared domain
        return 20.0, 1.0, 50
    if sid == 1062:                      # 6e10 shares: above the declared domain (still under the A9 ceiling)
        return 20.0, 1.0, 60_000_000
    if sid == 1064:                      # dividend payer whose crossing step ends after the break (2021-01-06)
        if FB_HOLE_1064[0] <= d <= FB_HOLE_1064[1]:
            return None
        return 20.0, 1.0 if old else 0.97, 1000
    if sid == 1065:                      # 1:10 consolidation, factor chained by the vendor, raw followed
        post = d >= FB_CONSOL_1065
        return (20.0 if post else 2.0), (1.0 if post else 10.0), (1000 if post else 10_000)
    if sid == 1066:                      # a count ~1000x too small inside the domain: 1.5e5 shares
        return 20.0, 1.0, 150
    if sid in (1067, 1068, 1069):
        return 20.0, 1.0, 1000
    # 1063: later 4:1 split (factor x1/4 at the break); no share count after 2020-11-30 (stale lag rows)
    return 20.0, 1.0 if old else 0.25, (1000 if d < dt.date(2020, 12, 1) else None)


def fb_volume(sid, d):
    """Raw daily share volume (each day's own share units)."""
    if sid == 1065:
        return 5e5 if d >= FB_CONSOL_1065 else 5e6      # 50%/day of 1e6 new / 1e7 old shares
    if sid == 1066:
        return 5e5                                        # 3.3x its (too small) share count every day
    if sid == 1068:
        return 5e6 if d >= FB_SPIKE_1068 else 1e4
    if sid == 1069:
        return 3e6                                        # exactly 3.0x its 1e6 shares: kept
    return 1e4


def write_fb_tickerhistory(path):
    cols = {k: [] for k in ("tradingDate", "securityID", "close", "volume", "shares", "cumulReturnFactor")}
    for d in FB_TH_DAYS:
        for sid in FB_IDS:
            row = fb_line(sid, d)
            if row is not None:
                for k, v in zip(cols, (d, sid, row[0], fb_volume(sid, d), row[2], row[1])):
                    cols[k].append(v)
    order = np.random.default_rng(11).permutation(len(cols["securityID"]))  # dates mixed in every group
    types = {"tradingDate": pa.date32(), "securityID": pa.int64(), "close": pa.float32(), "volume": pa.float64(),
             "shares": pa.int64(), "cumulReturnFactor": pa.float64()}
    pq.write_table(pa.table({k: pa.array([cols[k][i] for i in order], types[k]) for k in cols}), path, row_group_size=4000)


def write_axes_role(root, ids, sessions, source_sha, repair_sessions):
    """A role cut from the fixture's vendor rows (close = raw x factor, raw-share volume), optionally repaired.
    1062 (above the shares domain) is not a member on the first three sessions."""
    root.mkdir(parents=True)
    member = np.ones((len(sessions), len(ids)), dtype="u1")
    member[:3, ids.index(1062)] = 0
    raw = np.full((len(sessions), len(ids)), np.nan)
    close, volume = raw.copy(), raw.copy()
    for t, d in enumerate(sessions):
        for i, sid in enumerate(ids):
            row = fb_line(sid, d)
            if row is not None and row[0] is not None:
                raw[t, i] = float(np.float32(row[0]))
                close[t, i] = raw[t, i] * row[1]
                volume[t, i] = fb_volume(sid, d)
    blobs = {"sessions.i64": np.array([day(d) * DAY_NS for d in sessions], dtype="<i8").tobytes(),
             "ids.u64": np.array(ids, dtype="<u8").tobytes(),
             "member.u8": member.tobytes(), "present.u8": np.isfinite(raw).astype("u1").tobytes(),
             "close.f64": close.astype("<f8").tobytes(), "raw_close.f64": raw.astype("<f8").tobytes(),
             "volume.f64": volume.astype("<f8").tobytes()}
    files = {}
    for name, blob in blobs.items():
        (root / name).write_bytes(blob)
        files[name] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete", "dates": len(sessions),
                "instruments": len(ids), "instrument_namespace": "spiderrock.securityID", "score_begin": 0,
                "score_end": len(sessions), "source_sha256": source_sha, "files": files}
    if repair_sessions is not None:
        manifest["repair"] = {"rule": "factor-break-v1", "mass_sessions": [{"session": s} for s in repair_sessions]}
    (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return sha(root / "manifest.json")


def fb_lag_rows(sid):
    return [d for d in FB_TH_DAYS if (x := fb_line(sid, d)) is not None and x[0] is not None
            and x[2] is not None and 0 < x[2] <= 100_000_000]


def fb_si(sid, s):
    """si_shares visible at session s: latest row with available_at < s, at most 45 days old."""
    rows = sorted((dt.date.fromisoformat(a), float(v)) for i, a, v in FB_FINRA if i == sid)
    seen = [(a, v) for a, v in rows if a < s]
    return seen[-1][1] if seen and (s - seen[-1][0]).days <= 45 else math.nan


def fb_median_volume(sessions, t, sid):
    """Median raw volume of the name's present days among the 21 role sessions ending at t, each restated to
    session t's share basis by the vendor factor ratio; None with fewer than 11 present days."""
    import statistics
    here = fb_line(sid, sessions[t])
    days = [d for d in sessions[max(0, t - 20):t + 1] if (x := fb_line(sid, d)) is not None and x[0] is not None]
    if here is None or here[0] is None or len(days) < 11:
        return None
    return statistics.median(fb_volume(sid, d) * here[1] / fb_line(sid, d)[1] for d in days)


def fb_expected(sessions):
    """Independent oracle: lag observation's shares x GENUINE actions in (lag, session] x 1000, then the domain
    and the two units rules. Returns the matrix, the (restated, gap-ambiguous) cell counts and the rule counts."""
    import bisect
    out = np.full((len(sessions), len(FB_IDS)), np.nan)
    restated = ambiguous = 0
    rules = {"turnover": 0, "si": 0, "also_si": 0, "not_evaluable": 0, "si_not_evaluable": 0, "published": 0}
    for i, sid in enumerate(FB_IDS):
        rows = fb_lag_rows(sid)
        for t, s in enumerate(sessions):
            here = fb_line(sid, s)
            if here is None or here[0] is None:
                continue
            lag = s - dt.timedelta(days=90)
            k = bisect.bisect_right(rows, lag) - 1
            if k < 0 or rows[k] < lag - dt.timedelta(days=400):
                continue
            L = rows[k]
            val = fb_line(sid, L)[2] * 1000.0 * math.prod(r for e, r in FB_EVENTS.get(sid, []) if L < e <= s)
            if sid == 1056 and L < FB_GAP_END <= s:
                ambiguous += 1
                continue
            if not 1e5 <= val <= 5e10:
                continue
            was_restated = sid in FB_REPAIRED_IDS and L < FB_BREAK <= s
            restated += was_restated                       # counted before the units rules
            median = fb_median_volume(sessions, t, sid)
            si = fb_si(sid, s)
            si_high = si / val > 5.0                       # root ruling, fix round 4 (was 1.5)
            rules["not_evaluable"] += median is None
            rules["si_not_evaluable"] += math.isnan(si)
            if median is not None and median > 3.0 * val:  # root ruling, fix round 4 (was 1.0)
                rules["turnover"] += 1
                rules["also_si"] += si_high
                continue
            if si_high:
                rules["si"] += 1
                continue
            out[t, i] = val
            rules["published"] += was_restated
    return out, restated, ambiguous, rules


class FactorBreakShares(unittest.TestCase):
    """shares_out across an unchained vendor factor: correct on every session, point in time."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.base = Path(cls.temp.name)
        cls.th = cls.base / "th.parquet"
        write_fb_tickerhistory(cls.th)
        th_sha = sha(cls.th)
        cls.finra = cls.base / "finra"
        write_finra(cls.finra, FB_FINRA, [])
        short = [d for d in FB_SESSIONS if d <= dt.date(2021, 1, 5)]
        prespike = [d for d in FB_SESSIONS if d <= FB_SPIKE_1068 + dt.timedelta(days=9)]  # 8 spike sessions in
        cls.roles = {name: (sessions, write_axes_role(cls.base / f"role-{name}", FB_IDS, sessions, th_sha, repair))
                     for name, sessions, repair in (("fb", FB_SESSIONS, ["2021-01-04"]), ("post", FB_POST_SESSIONS, None),
                                                    ("short", short, ["2021-01-04"]), ("unrepaired", FB_SESSIONS, None),
                                                    ("wrong", FB_SESSIONS, ["2021-01-05"]),
                                                    ("starts", [d for d in FB_SESSIONS if d >= FB_BREAK], None),
                                                    ("prespike", prespike, ["2021-01-04"]))}
        cls.manifest = cls.run_role("fb")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @classmethod
    def run_role(cls, name, fields=("si_shares", "shares_out")):
        with contextlib.redirect_stdout(io.StringIO()):
            return tool.run(cls.base / f"role-{name}", cls.roles[name][1], cls.base / f"out-{name}", list(fields),
                            finra=cls.finra, tickerhistory=cls.th, lake=cls.base / "no-lake")

    def shares(self, name):
        return np.fromfile(self.base / f"out-{name}" / "shares_out.f64", dtype="<f8").reshape(
            len(self.roles[name][0]), len(FB_IDS))

    def test_restated_correctly_across_the_unchained_factor(self):
        got = self.shares("fb")
        expected, restated, ambiguous, rules = fb_expected(FB_SESSIONS)
        np.testing.assert_allclose(got, expected, rtol=1e-12, atol=0, equal_nan=True)
        col, t = FB_IDS.index, FB_SESSIONS.index
        s = t(dt.date(2021, 2, 1))                         # lag row 2020-11-03 is before the break
        self.assertAlmostEqual(got[s, col(1053)] / 1e6, 1.0, places=12)  # the raw factor ratio says x10
        self.assertAlmostEqual(got[s, col(1054)] / 1e6, 1.0, places=12)  # ... and x1/4
        self.assertAlmostEqual(got[t(dt.date(2021, 3, 1)), col(1054)] / 4e6, 1.0, places=12)  # genuine split kept
        self.assertAlmostEqual(got[s, col(1001)] / 1e6, 1.0, places=12)  # ... and 0.97 for a dividend payer
        self.assertAlmostEqual(got[s, col(1055)] / 2e6, 1.0, places=12)  # split on the break shown by the factor
        self.assertAlmostEqual(got[t(FB_BREAK), col(1054)] / 1e6, 1.0, places=12)  # corrected on the step's own day
        self.assertTrue(np.isnan(got[t(FB_NULL_RAW_1059), col(1059)]))  # no observation on the session
        self.assertAlmostEqual(got[t(dt.date(2021, 4, 6)), col(1064)] / 1e6, 1.0, places=12)  # lag row IS the step end
        self.assertTrue(np.all(np.isnan(got[:, col(1061)])) and np.all(np.isnan(got[:, col(1062)])))
        fb = next(f for f in self.manifest["fields"] if f["name"] == "shares_out")
        block = fb["factor_break"]
        self.assertEqual(block["mass_sessions"], [{"session": "2021-01-04", "inside_role": True, "jump_cells": 56,
                                                   "crossing_steps": 60, "repaired": 57, "kept_gap": 1,
                                                   "kept_split_follow": 1, "kept_distribution": 1}])
        self.assertEqual(block["role_repair_mass_sessions"], ["2021-01-04"])
        self.assertEqual((block["max_non_mass_jump_cells"], block["max_non_mass_session"]), (1, "2021-01-06"))  # 1064
        self.assertEqual(block["parameters"], tool.FB_PARAMETERS)
        self.assertEqual((block["restated_cells"], block["gap_ambiguous_to_nan_cells"]), (restated, ambiguous))
        self.assertEqual(block["restated_member_cells"], restated)  # every fixture cell is a member
        # restated_cells is counted before the units rules; 1053 loses cells to rule (b) inside its window.
        self.assertEqual((block["restated_published_cells"], block["restated_published_member_cells"]),
                         (rules["published"], rules["published"]))
        self.assertLess(rules["published"], restated)
        self.assertTrue(np.isnan(got[t(dt.date(2021, 2, 2)), col(1053)]))  # restated, then SI 6x: NaN
        self.assertGreater(restated, 0)
        self.assertGreater(ambiguous, 0)
        plaus = fb["plausibility"]
        n = len(FB_SESSIONS)
        units = rules["turnover"] + rules["si"]
        self.assertEqual((plaus["below_min"], plaus["above_max"], plaus["implausible_to_nan"]), (n, n, 2 * n + units))
        self.assertEqual(plaus["implausible_to_nan_member"], 2 * n - 3 + units)  # every units-rule cell is a member
        self.assertEqual((plaus["member_below_min"], plaus["member_above_max"]), (n, n - 3))
        self.assertTrue(fb["point_in_time"])
        self.assertEqual(fb["depends_on"], ["si_shares"])

    def test_units_rules_turnover_and_short_interest(self):
        got = self.shares("fb")
        _, _, _, rules = fb_expected(FB_SESSIONS)
        col, t = FB_IDS.index, FB_SESSIONS.index
        mis = got[:, col(1066)]                      # 1.5e5 shares trading 5e5 a day
        self.assertTrue(np.all(mis[:10] == 1.5e5))   # fewer than 11 window sessions: not evaluable, kept
        self.assertTrue(np.all(np.isnan(mis[10:])))
        spike = got[:, col(1068)]                    # 5x its shares a day from FB_SPIKE_1068
        first = t(FB_SPIKE_1068)
        self.assertTrue(np.all(spike[:first + 10] == 1e6))  # a spike day alone, or a minority of the window: kept
        self.assertTrue(np.all(np.isnan(spike[first + 10:])))  # the 11th spike session makes the median 5e6
        si = got[:, col(1067)]
        for s in FB_SESSIONS:
            ratio = fb_si(1067, s) / 1e6
            self.assertEqual(np.isnan(si[t(s)]), ratio > 5.0, s)  # 5.5 -> NaN; exactly 5.0 -> kept
        self.assertTrue(np.all(si[[t(s) for s in FB_SESSIONS if fb_si(1067, s) == 5e6]] == 1e6))
        self.assertTrue(np.all(got[:, col(1069)] == 1e6))  # median volume exactly 3.0x shares_out: kept
        cons = got[:, col(1065)]                     # consolidation: 50%/day in either share basis
        self.assertTrue(np.all(np.isfinite(cons)))  # raw volume unconverted would read 5x (5e6 vs 1e6) after it
        self.assertTrue(np.all(np.isfinite(got[:, col(1059)][[t(s) for s in FB_SESSIONS if s != FB_NULL_RAW_1059]])))
        block = next(f for f in self.manifest["fields"] if f["name"] == "shares_out")["plausibility"]
        self.assertEqual((block["turnover"]["to_nan"], block["turnover"]["to_nan_member"]),
                         (rules["turnover"], rules["turnover"]))
        self.assertEqual(block["turnover"]["also_above_si_ratio"], rules["also_si"])
        self.assertEqual(block["turnover"]["not_evaluable_cells"], rules["not_evaluable"])
        self.assertEqual((block["si_ratio"]["to_nan"], block["si_ratio"]["to_nan_member"]), (rules["si"], rules["si"]))
        self.assertEqual((block["turnover"]["window_sessions"], block["turnover"]["min_present_sessions"],
                          block["turnover"]["max_median_volume_over_shares_out"],
                          block["si_ratio"]["max_si_shares_over_shares_out"]), (21, 11, 3.0, 5.0))
        self.assertEqual(block["si_ratio"]["not_evaluable_cells"], rules["si_not_evaluable"])
        self.assertGreater(rules["si_not_evaluable"], 0)
        self.assertGreater(rules["also_si"], 0)
        self.assertEqual([Path(x["path"]).name for x in block["turnover"]["inputs"]],
                         ["volume.f64", "close.f64", "raw_close.f64", "present.u8"])
        # si_shares itself is never touched by the units rules.
        si_field = np.fromfile(self.base / "out-fb" / "si_shares.f64", dtype="<f8").reshape(len(FB_SESSIONS), len(FB_IDS))
        self.assertEqual(si_field[t(dt.date(2021, 2, 2)), col(1067)], 5.5e6)

    def test_units_rules_point_in_time(self):
        # A role ending 8 spike sessions in: the later spike (and later rows) change no earlier cell.
        self.run_role("prespike")
        pre = self.shares("prespike")
        np.testing.assert_array_equal(pre, self.shares("fb")[:len(pre)])
        self.assertTrue(np.all(pre[:, FB_IDS.index(1068)] == 1e6))

    def test_shares_out_requires_si_shares(self):
        with self.assertRaisesRegex(ValueError, "shares_out requires si_shares"):
            self.run_role("fb", fields=("shares_out",))  # refused before the (existing) output is touched

    def test_break_before_the_role_corrects_stale_lag_rows(self):
        m = self.run_role("post")
        got = self.shares("post")
        expected, restated, _, _ = fb_expected(FB_POST_SESSIONS)
        np.testing.assert_allclose(got, expected, rtol=1e-12, atol=0, equal_nan=True)
        self.assertTrue(np.allclose(got[:, FB_IDS.index(1063)], 1e6, rtol=1e-12))  # lag 2020-11-30: x1/4 undone
        block = next(f for f in m["fields"] if f["name"] == "shares_out")["factor_break"]
        self.assertEqual([(x["session"], x["inside_role"]) for x in block["mass_sessions"]], [("2021-01-04", False)])
        self.assertEqual(block["role_repair_mass_sessions"], [])
        self.assertEqual(block["restated_cells"], restated)
        self.assertEqual(restated, len(FB_POST_SESSIONS))  # only 1063's stale lag row precedes the break

    def test_point_in_time_later_rows_change_no_earlier_cell(self):
        self.run_role("short")  # ends 2021-01-05: 1056's post-hole row (2021-01-06) is not yet known
        short = self.shares("short")
        np.testing.assert_array_equal(short, self.shares("fb")[:len(short)])

    def test_break_on_the_first_session_needs_no_role_repair(self):
        # No return across the role's first session is in the role, so an unrepaired role starting on the break
        # is consistent; shares_out still divides the step out of lag rows before it.
        m = self.run_role("starts")
        sessions = self.roles["starts"][0]
        np.testing.assert_allclose(self.shares("starts"), fb_expected(sessions)[0], rtol=1e-12, atol=0, equal_nan=True)
        block = next(f for f in m["fields"] if f["name"] == "shares_out")["factor_break"]
        self.assertEqual([(x["session"], x["inside_role"]) for x in block["mass_sessions"]], [("2021-01-04", False)])

    def test_role_repair_block_must_match_detected_breaks(self):
        for name in ("unrepaired", "wrong"):
            with self.assertRaisesRegex(ValueError, r"factor-break-v1: mass sessions inside the role \['2021-01-04'\]"):
                self.run_role(name)
            self.assertFalse((self.base / f"out-{name}" / "manifest.json").exists())


# ---------------------------------------------------------------------------------------------------------
# Issuer-level fields (T21): identity bridge (T19 rows) + fundamental events and SIC events (T20 snapshots).
# The main fixture's role (2024-10-01 .. 2024-12-31, ids 101..505) with an independent pure-Python oracle.
# ---------------------------------------------------------------------------------------------------------

UTC = dt.timezone.utc
OPEN = None


def at(y, m, d, hh=0, mm=0, us=0):
    return dt.datetime(y, m, d, hh, mm, tzinfo=UTC) + dt.timedelta(microseconds=us)


def mark(d):
    return dt.datetime.combine(d, dt.time(22), tzinfo=UTC)


LONG_AGO = mark(dt.date(2015, 1, 1))  # T19 invariant: available_at <= start 22:00 UTC (here exactly at it)
# (sr_id, cik, start, end_incl, available_at, primary, tier, basis)
BRIDGE = [
    (101, 1001, dt.date(2015, 1, 1), OPEN, LONG_AGO, "P", "high", "reconstructed_high"),
    (303, 1001, dt.date(2015, 1, 1), OPEN, LONG_AGO, "J", "high", "reconstructed_high"),       # second class of 1001
    (202, 2002, dt.date(2024, 10, 15), dt.date(2024, 12, 2), mark(dt.date(2024, 10, 15)), "P", "medium",
     "reconstructed_medium"),                                  # available exactly at its start mark: 10-15 included
    (404, 4004, dt.date(2015, 1, 1), OPEN, LONG_AGO, "P", "high", "reconstructed_high"),
    (404, 4005, dt.date(2024, 11, 1), dt.date(2024, 11, 29), LONG_AGO, "P", "high", "reconstructed_high"),  # ambiguous
    (505, 5005, dt.date(2015, 1, 1), OPEN, at(2024, 10, 20, 12), "P", "high",
     "reconstructed_high"),                                    # violates the invariant: counted, gated to 10-21
    (999, 9009, dt.date(2015, 1, 1), OPEN, LONG_AGO, "P", "high", "reconstructed_high"),       # off the role axis
    (505, 5006, dt.date(2015, 1, 1), OPEN, LONG_AGO, "N", "low", "reconstructed_medium"),      # N: dropped
    (202, 2003, dt.date(2024, 10, 1), OPEN, LONG_AGO, "P", "high", "current_ticker_verified"),  # excluded basis
    (101, 1009, dt.date(2015, 1, 1), OPEN, at(2025, 2, 1), "P", "high", "reconstructed_high"),  # sealed
]
# (cik, accepted_utc, clock_basis, period_end, staleness_days, be, at, accession)
EVENTS = [
    (1001, at(2024, 6, 1, 12), "fsds_accepted_utc", dt.date(2024, 3, 31), 200, 100.0, 1000.0, "0000001001-24-000001"),
    (1001, mark(dt.date(2024, 10, 10)) - dt.timedelta(microseconds=1), "fsds_accepted_utc", dt.date(2024, 6, 30), 200,
     110.0, 1100.0, "0000001001-24-000002"),
    (1001, mark(dt.date(2024, 10, 21)), "cf_fc1", dt.date(2024, 6, 30), 200, 111.0, 1101.0,
     "0000001001-24-000003"),                                                    # restated, clock exactly at a mark
    (1001, at(2024, 11, 12, 21), "fsds_accepted_utc", dt.date(2024, 9, 30), 200, math.nan, 1300.0,
     "0000001001-24-000004"),                                                    # visible NaN be
    (1001, at(2025, 1, 5, 12), "fsds_accepted_utc", dt.date(2024, 12, 31), 200, 999.0, 9999.0,
     "0000001001-25-000001"),                                                    # sealed
    (2002, at(2024, 5, 10, 20), "fsds_accepted_utc", dt.date(2024, 4, 15), 200, 50.0, 500.0,
     "0000002002-24-000001"),                                                    # stale after 2024-11-01
    (4004, at(2024, 8, 1, 20), "fsds_accepted_utc", dt.date(2024, 6, 30), 200, 40.0, 400.0, "0000004004-24-000001"),
    (4004, at(2024, 9, 3, 20), "fsds_accepted_utc", dt.date(2024, 6, 30), 200, 42.0, 420.0,
     "0000004004-24-000009"),                                                    # same CIK and clock: the larger
    (4004, at(2024, 9, 3, 20), "fsds_accepted_utc", dt.date(2024, 6, 30), 200, 41.0, 410.0,
     "0000004004-24-000002"),                                                    # accession wins, not file order
    (4005, at(2024, 8, 1, 20), "fsds_accepted_utc", dt.date(2024, 6, 30), 200, 45.0, 450.0, "0000004005-24-000001"),
    (5005, at(2024, 1, 20, 20), "cf_fc1", dt.date(2023, 10, 1), 400, 55.0, 550.0,
     "0000005005-24-000001"),                                                    # annual-only filer: 400 days
    (7777, at(2024, 8, 1, 20), "fsds_accepted_utc", dt.date(2024, 6, 30), 200, 77.0, 770.0,
     "0000007777-24-000001"),                                                    # unlinked CIK
]
# (cik, accepted_utc, sic, accession)
SIC_EVENTS = [
    (1001, at(2024, 1, 10, 15), 7372, "0000001001-24-000000"),
    (1001, at(2024, 10, 24, 15), 3674, "0000001001-24-000005"),  # SIC change: visible from 10-25 (lag 1)
    (1001, at(2025, 2, 1, 15), 1000, "0000001001-25-000002"),    # sealed
    (2002, at(2023, 5, 1, 12), 6022, "0000002002-23-000001"),    # 550 days old on 2024-11-01
    (4004, at(2024, 2, 1, 12), 9999, "0000004004-24-000000"),    # no FF49 industry
    (5005, at(2024, 3, 1, 12), 2834, "0000005005-24-000002"),
    (5005, at(2024, 6, 1, 12), None, "0000005005-24-000003"),    # carries no SIC: skipped (guard)
    (4005, at(2024, 3, 1, 12), 0, "0000004005-24-000000"),       # invalid: skipped (guard)
]
ITEM_NAMES = [x[0] for x in tool.FUND_ITEMS]


def item_value(item, row, be, at_):
    if item == "be":
        return be
    if item == "at":
        return at_
    return float(1000 * ITEM_NAMES.index(item) + row)


def write_published(root, schema, tables, **flags):
    root.mkdir(parents=True)
    files = {}
    for name, table in tables.items():
        pq.write_table(table, root / name)
        files[name] = {"bytes": (root / name).stat().st_size, "sha256": sha(root / name), "rows": table.num_rows}
    (root / "manifest.json").write_text(json.dumps({"schema": schema, "status": "complete", "files": files, **flags}),
                                        encoding="utf-8")
    return sha(root / "manifest.json")


def naive(values):
    return [x.replace(tzinfo=None) for x in values]


def write_bridge(root, rows=BRIDGE, **flags):
    """The T19 contract: links.parquet with a naive-UTC timestamp[us] available_at."""
    flags.setdefault("rehearsal_identity", True)
    flags.setdefault("source", {"scope_complete": False})
    cols = list(zip(*rows))
    table = pa.table({"sr_id": pa.array(cols[0], pa.int64()), "cik": pa.array(cols[1], pa.int64()),
                      "start": pa.array(cols[2], pa.date32()), "end_incl": pa.array(cols[3], pa.date32()),
                      "available_at": pa.array(naive(cols[4]), pa.timestamp("us")),
                      "primary": pa.array(cols[5], pa.string()), "tier": pa.array(cols[6], pa.string()),
                      "basis": pa.array(cols[7], pa.string())})
    return write_published(root, "atx.identity-bridge/v1", {"links.parquet": table}, **flags)


def write_events(root, events=EVENTS, sic_events=SIC_EVENTS, drop_items=(), staleness=None):
    """The T20 contract: fundamental_events.parquet and sic_events.parquet (accepted_utc timestamp[us, UTC])."""
    cols = list(zip(*events))
    data = {"cik": pa.array(cols[0], pa.int64()), "accession": pa.array(cols[7], pa.string()),
            "accepted_utc": pa.array(cols[1], pa.timestamp("us", tz="UTC")),
            "clock_basis": pa.array(cols[2], pa.string()), "period_end": pa.array(cols[3], pa.date32()),
            "staleness_days": pa.array(staleness or cols[4], pa.int32())}
    for item in ITEM_NAMES:
        if item not in drop_items:
            data[item] = pa.array([item_value(item, r, e[5], e[6]) for r, e in enumerate(events)], pa.float64())
    s = list(zip(*sic_events))
    sic = pa.table({"cik": pa.array(s[0], pa.int64()), "accession": pa.array(s[3], pa.string()),
                    "accepted_utc": pa.array(s[1], pa.timestamp("us", tz="UTC")),
                    "clock_basis": pa.array(["fsds_accepted_utc"] * len(s[0]), pa.string()),
                    "sic": pa.array(s[2], pa.int32())})
    return write_published(root, "atx.fundamental-events/v1",
                           {"fundamental_events.parquet": pa.table(data), "sic_events.parquet": sic},
                           values_label="modeled_unaccepted", rehearsal_identity=True, items=ITEM_NAMES)


def oracle_link(sid, d):
    """(cik or None or 'amb', primary) of line sid at session d: the interval, plus the <= gate (binds only on rows
    that violate available_at <= start mark)."""
    q = [(cik, kind) for s, cik, start, end, avail, kind, _, basis in BRIDGE
         if s == sid and kind in ("P", "J") and basis != "current_ticker_verified" and avail < at(2025, 1, 1)
         and start <= d and (end is None or d <= end) and avail <= mark(d)]
    ciks = {c for c, _ in q}
    if not ciks:
        return None, False
    if len(ciks) > 1:
        return "amb", False
    c = ciks.pop()
    return c, any(k == "P" for _, k in q)


def oracle_latest(rows, cik, cutoff, accession):
    """Index of the CIK's row with clock < cutoff (not sealed) and the largest (clock, accession)."""
    best = None
    for r, row in enumerate(rows):
        if row[0] == cik and row[1] < cutoff and row[1] < at(2025, 1, 1):
            if best is None or (row[1], row[accession]) > (rows[best][1], rows[best][accession]):
                best = r
    return best


def oracle_fund(item, lag):
    out = np.full((len(SESSIONS), len(IDS)), np.nan)
    for t, d in enumerate(SESSIONS):
        for i, sid in enumerate(IDS):
            cik, primary = oracle_link(sid, d)
            if not primary or t < lag:
                continue
            r = oracle_latest(EVENTS, cik, mark(SESSIONS[t - lag]), 7)
            if r is None:
                continue
            _, _, _, pe, stale_days, be, at_, _ = EVENTS[r]
            if (d - pe).days <= stale_days:
                out[t, i] = item_value(item, r, be, at_)
    return out


FF = {7372: (73, 6, 36), 3674: (36, 6, 37), 6022: (60, 11, 45), 9999: (99, 12, math.nan), 2834: (28, 10, 13)}


def oracle_grp(kind, lag):
    valid = [row for row in SIC_EVENTS if row[2] is not None and 100 <= row[2] <= 9999]
    out = np.full((len(SESSIONS), len(IDS)), np.nan)
    for t, d in enumerate(SESSIONS):
        for i, sid in enumerate(IDS):
            cik, primary = oracle_link(sid, d)
            if not primary or t < lag:
                continue
            r = oracle_latest(valid, cik, mark(SESSIONS[t - lag]), 3)
            if r is not None and (d - valid[r][1].date()).days <= 550:
                out[t, i] = FF[valid[r][2]][("sic2", "ff12", "ff49").index(kind)]
    return out


def oracle_me(shares_out, raw, present):
    out = np.full((len(SESSIONS), len(IDS)), np.nan)
    for t, d in enumerate(SESSIONS):
        links = {sid: oracle_link(sid, d) for sid in IDS}
        for i, sid in enumerate(IDS):
            cik, primary = links[sid]
            if not primary:
                continue
            terms = [shares_out[t, j] * raw[t, j] if present[t, j] else math.nan
                     for j, s in enumerate(IDS) if links[s][0] == cik]
            if all(math.isfinite(x) and x > 0 for x in terms):
                out[t, i] = sum(terms)
    return out


ISSUER_RUN = list(tool.DEFAULT_FIELDS) + list(tool.ISSUER_FIELDS)


class IssuerFields(unittest.TestCase):
    """fund / me_company / grp fields: link rule, lagged filing clock, staleness, restatements, P-only, PIT."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.fx = Fixture(Path(cls.temp.name))
        cls.bridge, cls.events = cls.fx.base / "bridge", cls.fx.base / "events"
        cls.bridge_sha, cls.events_sha = write_bridge(cls.bridge), write_events(cls.events)
        cls.legacy = cls.fx.run("legacy")                      # default fields only
        cls.manifest = cls.produce("issuer", ISSUER_RUN)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @classmethod
    def produce(cls, out, fields, **kw):
        kw.setdefault("identity_bridge", cls.bridge)
        kw.setdefault("identity_bridge_sha256", cls.bridge_sha)
        kw.setdefault("fund_events", cls.events)
        kw.setdefault("fund_events_sha256", cls.events_sha)
        return cls.fx.run(out, fields=list(fields), **kw)

    def entry(self, name, manifest=None):
        return next(f for f in (manifest or self.manifest)["fields"] if f["name"] == name)

    def test_legacy_fields_byte_identical_and_defaults_unchanged(self):
        self.assertEqual(tool.DEFAULT_FIELDS, ("si_shares", "si_dtc", "iv_atm_21d", "iv_atm_63d", "iv_atm_126d",
                                               "earn_recent", "shares_out", "mkt_ret"))
        self.assertFalse(set(tool.ISSUER_FIELDS) & set(tool.FIELDS))
        self.assertEqual(len(tool.DEFAULT_FIELDS) + len(tool.ISSUER_FIELDS), 40)  # <= 64 runner fields
        for name in tool.DEFAULT_FIELDS:
            self.assertEqual(self.manifest["files"][f"{name}.f64"], self.legacy["files"][f"{name}.f64"], name)
            self.assertEqual(self.entry(name), self.entry(name, self.legacy), name)
        self.assertNotIn("issuer", self.legacy["source_checks"])
        self.assertEqual([e["name"] for e in self.manifest["fields"]], ISSUER_RUN)  # legacy first, registry order
        for e in self.manifest["fields"]:
            if e["name"] in tool.ISSUER_FIELDS:
                self.assertIs(e["point_in_time"], True)
                self.assertEqual(e["non_pit_aspects"], [])
                self.assertTrue(e["definition"])
                self.assertNotIn("{lag}", e["clock"])
        self.assertEqual(self.manifest["non_point_in_time_fields"], [])
        self.assertEqual(self.entry("me_company")["depends_on"], ["shares_out"])

    def test_fund_link_rule_lagged_clock_restatement_staleness(self):
        for item in ("be", "at", "sue"):
            np.testing.assert_array_equal(self.fx.field("issuer", item), oracle_fund(item, 1), item)
        be = self.fx.field("issuer", "be")
        a, b, c, d, e = range(5)
        self.assertTrue(np.all(np.isnan(be[0])))                  # t < lag: nothing visible yet
        self.assertEqual(be[1, a], 100)
        self.assertEqual(be[t_of("2024-10-10"), a], 100)          # clock 1 us before the 10-10 mark: s0 = 10-10 ...
        self.assertEqual(be[t_of("2024-10-11"), a], 110)          # ... + 1 declared lag session
        self.assertEqual(be[t_of("2024-10-22"), a], 110)          # restatement clock == the 10-21 mark: s0 = 10-22
        self.assertEqual(be[t_of("2024-10-23"), a], 111)          # restated value from s0 + 1, never backdated
        self.assertTrue(np.isnan(be[t_of("2024-11-13"), a]))      # visible NaN in the latest row: no skip-back
        self.assertEqual(self.fx.field("issuer", "at")[t_of("2024-11-13"), a], 1300)
        self.assertTrue(np.all(np.isnan(be[:, c])))               # J line: issuer fields are primary-only
        self.assertTrue(np.isnan(be[t_of("2024-10-14"), b]))      # before the link's start
        self.assertEqual(be[t_of("2024-10-15"), b], 50)           # available_at == start mark: first day kept
        self.assertTrue(np.isnan(be[t_of("2024-10-18"), e]))      # invariant violation gated: not before ...
        self.assertEqual(be[t_of("2024-10-21"), e], 55)           # ... the first mark >= available_at
        self.assertEqual(be[t_of("2024-11-01"), b], 50)           # age 200 (period end 2024-04-15): kept
        self.assertTrue(np.isnan(be[t_of("2024-11-04"), b]))      # age 203: stale
        self.assertEqual(be[t_of("2024-10-31"), d], 42)           # same CIK and clock: the later file row wins
        self.assertTrue(np.all(np.isnan(be[[t for t, x in enumerate(SESSIONS) if x.month == 11], d])))  # ambiguous
        self.assertEqual(be[t_of("2024-12-02"), d], 42)
        self.assertEqual(be[t_of("2024-11-04"), e], 55)           # annual-only: age 400 kept
        self.assertTrue(np.isnan(be[t_of("2024-11-05"), e]))      # age 401: stale
        r = self.entry("be")["nan_reasons_member_cells"]
        self.assertGreater(r["stale"], 0)
        self.assertGreater(r["visible_nan"], 0)
        self.assertGreater(r["no_visible_row"], 0)
        member = np.fromfile(self.fx.role / "member.u8", dtype="u1").reshape(len(SESSIONS), len(IDS)) != 0
        fc1 = np.zeros_like(member)
        fc1[t_of("2024-10-23"):t_of("2024-11-13"), a] = True      # 1001's FC1 restatement row is visible
        fc1[t_of("2024-10-21"):t_of("2024-11-05"), e] = True      # 5005's FC1 row (linked from 10-21)
        fc1[0] = False
        self.assertEqual(r["fc1_finite_member_cells"], int(np.count_nonzero(fc1 & member & np.isfinite(be))))

    def test_lag_zero_shifts_by_one_session_and_is_declared_in_the_clock(self):
        m = self.produce("lag0", ["be", "grp_ff49"], fund_lag_sessions=0)
        be = self.fx.field("lag0", "be")
        np.testing.assert_array_equal(be, oracle_fund("be", 0))
        np.testing.assert_array_equal(self.fx.field("lag0", "grp_ff49"), oracle_grp("ff49", 0))
        self.assertEqual(be[t_of("2024-10-10"), 0], 110)
        self.assertEqual(be[t_of("2024-10-22"), 0], 111)
        self.assertNotEqual(self.entry("be", m)["clock"], self.entry("be")["clock"])  # runner refuses mixed lags
        self.assertIn("L = 1", self.entry("be")["clock"])
        self.assertEqual(self.manifest["source_checks"]["issuer"]["fund_lag_sessions"], 1)

    def test_group_fields_sic_change_staleness_and_unmapped(self):
        for kind in ("sic2", "ff12", "ff49"):
            np.testing.assert_array_equal(self.fx.field("issuer", f"grp_{kind}"), oracle_grp(kind, 1), kind)
        ff49, ff12 = self.fx.field("issuer", "grp_ff49"), self.fx.field("issuer", "grp_ff12")
        self.assertEqual(ff49[t_of("2024-10-24"), 0], 36)        # Softw (7372)
        self.assertEqual(ff49[t_of("2024-10-25"), 0], 37)        # Chips (3674) from s0 + 1
        self.assertEqual(ff12[t_of("2024-11-01"), 1], 11)        # Money: 6022 accepted 2023-05-01, age 550
        self.assertTrue(np.isnan(ff12[t_of("2024-11-04"), 1]))   # age 553: stale
        self.assertEqual(ff12[t_of("2024-10-31"), 3], 12)        # 9999: FF12 Other ...
        self.assertTrue(np.isnan(ff49[t_of("2024-10-31"), 3]))   # ... and no FF49 industry
        self.assertEqual(self.fx.field("issuer", "grp_sic2")[t_of("2024-10-22"), 4], 28)  # null-SIC row skipped
        st = self.manifest["source_checks"]["issuer"]
        self.assertEqual(st["sic_events"]["rows_used_invalid_sic_skipped"], 2)
        self.assertEqual(st["sic_events"]["rows_available_on_or_after_2025_dropped"], 1)
        self.assertGreater(self.entry("grp_ff49")["nan_reasons_member_cells"]["unmapped"], 0)
        self.assertEqual(st["sic_mapping"]["versions"], tool.SIC_MAPPING_VERSIONS)

    def test_sic_mapping_tables_match_the_atx_db_helpers(self):
        ff12, ff49, prov = tool.sic_mapping()
        from atx_db import reference_classifications as rc
        for sic in range(0, 10000):
            if sic < 100:
                self.assertTrue(np.isnan(ff12[sic]) and np.isnan(ff49[sic]))
                continue
            self.assertEqual(ff12[sic], tool.FF12_NUMBERS[rc.fama_french_12_for_sic(sic)])
            code = rc.fama_french_49_for_sic(sic)
            if code is None:
                self.assertTrue(np.isnan(ff49[sic]))
            else:
                self.assertEqual(rc.FF49_INDUSTRIES[int(ff49[sic]) - 1][1], code)
        self.assertEqual(sorted(set(ff12[100:].tolist())), list(range(1, 13)))
        self.assertEqual(sorted(set(ff49[np.isfinite(ff49)].tolist())), list(range(1, 50)))
        self.assertEqual(prov["table_sha256"], hashlib.sha256(np.stack([ff12, ff49]).astype("<f8").tobytes()).hexdigest())

    def test_me_company_line_summed_over_p_and_j_lines(self):
        close, raw, present = role_prices(len(SESSIONS))
        so = self.fx.field("issuer", "shares_out")
        got = self.fx.field("issuer", "me_company")
        np.testing.assert_allclose(got, oracle_me(so, raw, present), rtol=1e-15, atol=0, equal_nan=True)
        t = t_of("2024-10-15")
        self.assertAlmostEqual(got[t, 0], so[t, 0] * raw[t, 0] + so[t, 2] * raw[t, 2], delta=1e-6)
        self.assertTrue(np.all(np.isnan(got[:, 2])))               # J line itself: NaN
        after = [t for t, d in enumerate(SESSIONS) if d >= C81_303]
        self.assertTrue(np.all(np.isnan(got[after, 0])))         # 303 (J) shares_out withheld -> 1001 NaN
        self.assertTrue(np.all(np.isfinite(so[after, 0])))
        self.assertTrue(np.isnan(got[40, 4]))                     # 505 absent on session 40
        r = self.entry("me_company")["nan_reasons_member_cells"]
        member = np.fromfile(self.fx.role / "member.u8", dtype="u1").reshape(len(SESSIONS), len(IDS)) != 0
        self.assertEqual(r["other_linked_line_nan"], int(np.count_nonzero(member[after, 0])))  # 101 not a member at 60
        self.assertGreater(r["multi_line_finite"], 0)
        self.assertEqual([Path(s["path"]).name for s in self.entry("me_company")["sources"][-2:]],
                         ["raw_close.f64", "present.u8"])

    def test_link_and_event_accounting(self):
        st = self.manifest["source_checks"]["issuer"]
        b = st["identity_bridge"]
        self.assertEqual((b["rows_total"], b["rows_dropped_kind_not_p_or_j"], b["rows_dropped_excluded_basis"],
                          b["rows_available_on_or_after_2025_dropped"], b["rows_ignored_off_axis"], b["rows_used"]),
                         (10, 1, 1, 1, 1, 6))
        self.assertEqual((b["rehearsal_identity"], b["scope_complete"]), (True, False))
        self.assertEqual((b["rows_used_available_after_start_mark"], b["rows_used_available_exactly_at_start_mark"]),
                         (1, 4))
        self.assertIn("start <= date(session) <= end_incl", b["link_rule"])
        self.assertEqual(b["ambiguous_cells"], sum(d.month == 11 for d in SESSIONS))
        ev = st["fund_events"]
        self.assertEqual((ev["rows_total"], ev["rows_available_on_or_after_2025_dropped"], ev["rows_ignored_unlinked_cik"],
                          ev["rows_sharing_cik_and_clock"], ev["rows_used_fc1_clock"], ev["rows_used_staleness_400"]),
                         (12, 1, 1, 1, 2, 1))
        self.assertEqual((ev["values_label"], ev["rehearsal_identity"]), ("modeled_unaccepted", True))
        self.assertEqual(st["fund_events_manifest"]["items"], ITEM_NAMES)
        lc = st["link_member_cells"]
        self.assertEqual(lc["member_cells"], lc["unlinked"] + lc["ambiguous"] + lc["secondary"] + lc["primary"])
        names = {Path(s["path"]).name for s in self.entry("be")["sources"]}
        self.assertEqual(names, {"manifest.json", "links.parquet", "fundamental_events.parquet"})
        self.assertEqual({Path(s["path"]).name for s in self.entry("grp_ff12")["sources"]},
                         {"manifest.json", "links.parquet", "sic_events.parquet"})

    def test_link_rule_interval_only(self):
        # Controller ruling: rows meeting T19's invariant match on start <= d <= end_incl only (no strict
        # available_at < mark filter that would drop their first day). Checked on a bridge of compliant rows only.
        base = self.fx.base / "interval-only"
        compliant = [r for r in BRIDGE if not (r[0] == 505 and r[1] == 5005)]
        sha_ = write_bridge(base, compliant)
        m = self.produce("interval-only-out", ["be"], identity_bridge=base, identity_bridge_sha256=sha_)
        got = self.fx.field("interval-only-out", "be")
        for t, d in enumerate(SESSIONS):
            for i, sid in enumerate(IDS):
                rows = [r for r in compliant if r[0] == sid and r[5] in ("P", "J") and r[7] != "current_ticker_verified"
                        and r[4] < at(2025, 1, 1) and r[2] <= d and (r[3] is None or d <= r[3])]
                if not rows:
                    self.assertTrue(np.isnan(got[t, i]), (d, sid))  # no interval contains d
        np.testing.assert_array_equal(got[:, :4], self.fx.field("issuer", "be")[:, :4])
        self.assertTrue(np.all(np.isnan(got[:, 4])))
        b = m["source_checks"]["issuer"]["identity_bridge"]
        self.assertEqual((b["rows_used_available_after_start_mark"], b["rows_used_available_exactly_at_start_mark"]),
                         (0, 4))

    def test_point_in_time_later_rows_change_no_earlier_cell(self):
        late = mark(dt.date(2024, 12, 10)) - dt.timedelta(microseconds=1)
        extra = EVENTS + [(1001, late, "fsds_accepted_utc", dt.date(2024, 9, 30), 200, 123.0, 1234.0,
                           "0000001001-24-000009")]
        sic_extra = SIC_EVENTS + [(1001, late, 2834, "0000001001-24-000009")]
        root = self.fx.base / "events-late"
        sha_late = write_events(root, extra, sic_extra)
        self.produce("late", ["be", "grp_ff49"], fund_events=root, fund_events_sha256=sha_late)
        t = t_of("2024-12-11")
        for name in ("be", "grp_ff49"):
            before, after = self.fx.field("issuer", name), self.fx.field("late", name)
            np.testing.assert_array_equal(after[:t], before[:t], name)
            self.assertNotEqual(after[t, 0], before[t, 0], name)
        self.assertEqual(self.fx.field("late", "be")[t, 0], 123.0)

    def test_deterministic_rerun(self):
        again = self.produce("issuer-again", ISSUER_RUN)
        self.assertEqual(again, self.manifest)
        for name in list(self.manifest["files"]) + ["manifest.json"]:
            self.assertEqual(sha(self.fx.base / "issuer" / name), sha(self.fx.base / "issuer-again" / name))

    def test_refusals_fail_closed(self):
        cases = [
            ("no-bridge", ["be"], {"identity_bridge": None}, "identity-bridge"),
            ("no-events", ["be"], {"fund_events": None}, "fund-events"),
            ("me-only", ["me_company"], {}, "requires shares_out"),
            ("bad-lag", ["be"], {"fund_lag_sessions": 9}, "fund-lag-sessions"),
            ("bad-bridge-sha", ["be"], {"identity_bridge_sha256": "0" * 64}, "identity-bridge manifest SHA-256"),
            ("bad-events-sha", ["grp_ff12"], {"fund_events_sha256": "0" * 64}, "fund-events manifest SHA-256"),
        ]
        for out, fields, kw, pattern in cases:
            with self.assertRaisesRegex(ValueError, pattern):
                self.produce(out, fields, **kw)
            self.assertFalse((self.fx.base / out / "manifest.json").exists(), out)
        # me_company needs no events artifact.
        m = self.produce("me-no-events", ["si_shares", "shares_out", "me_company"], fund_events=None)
        self.assertEqual([e["name"] for e in m["fields"]], ["si_shares", "shares_out", "me_company"])
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            missing_sha = write_events(base / "missing", drop_items=("xrd_ttm",))
            with self.assertRaisesRegex(ValueError, r"lacks requested column\(s\) \['xrd_ttm'\]"):
                self.produce("missing-item", ["be", "xrd_ttm"], fund_events=base / "missing", fund_events_sha256=missing_sha)
            self.produce("other-items", ["be"], fund_events=base / "missing", fund_events_sha256=missing_sha)  # not needed
            flag_sha = write_bridge(base / "noflag", rehearsal_identity="yes")
            with self.assertRaisesRegex(ValueError, "rehearsal_identity"):
                self.produce("noflag", ["be"], identity_bridge=base / "noflag", identity_bridge_sha256=flag_sha)
            odd_sha = write_events(base / "odd-staleness", staleness=[300] * len(EVENTS))
            with self.assertRaisesRegex(ValueError, r"staleness_days outside the declared \[200, 400\]"):
                self.produce("odd-staleness", ["be"], fund_events=base / "odd-staleness", fund_events_sha256=odd_sha)
            schema_sha = write_published(base / "wrong-schema", "atx.identity-bridge/v0", {}, rehearsal_identity=True)
            with self.assertRaisesRegex(ValueError, "not a complete atx.identity-bridge/v1 artifact"):
                self.produce("wrong-schema", ["be"], identity_bridge=base / "wrong-schema",
                             identity_bridge_sha256=schema_sha)
            nocol_sha = write_published(base / "no-accession", "atx.fundamental-events/v1", {
                "fundamental_events.parquet": pa.table({"cik": pa.array([1001], pa.int64())}),
                "sic_events.parquet": pa.table({"cik": pa.array([1001], pa.int64())})})
            with self.assertRaisesRegex(ValueError, r"lacks contract column\(s\)"):
                self.produce("no-accession", ["grp_ff12"], fund_events=base / "no-accession", fund_events_sha256=nocol_sha)
            bad_sha = write_events(base / "tampered")
            path = base / "tampered" / "fundamental_events.parquet"
            path.write_bytes(path.read_bytes() + b"\0")
            with self.assertRaisesRegex(ValueError, "does not match its manifest entry"):
                self.produce("tampered", ["be"], fund_events=base / "tampered", fund_events_sha256=bad_sha)
            self.assertFalse((self.fx.base / "tampered" / "manifest.json").exists())

    def test_clock_units_and_empty_inputs(self):
        # tz-aware ns bridge clocks and naive ms event clocks read as the same UTC instants.
        base = self.fx.base / "variants"
        c = list(zip(*BRIDGE))
        bridge_sha = write_published(base / "bridge", "atx.identity-bridge/v1", {"links.parquet": pa.table({
            "sr_id": pa.array(c[0], pa.int64()), "cik": pa.array(c[1], pa.int64()),
            "start": pa.array(c[2], pa.date32()), "end_incl": pa.array(c[3], pa.date32()),
            "available_at": pa.array(c[4], pa.timestamp("ns", tz="UTC")), "primary": pa.array(c[5], pa.string()),
            "basis": pa.array(c[7], pa.string())})}, rehearsal_identity=True)
        events_sha = write_events(base / "events")
        table = pq.read_table(base / "events" / "fundamental_events.parquet")
        i = table.schema.get_field_index("accepted_utc")
        table = table.set_column(i, "accepted_utc", pa.array(naive(list(zip(*EVENTS))[1]), pa.timestamp("us")))
        pq.write_table(table, base / "events" / "fundamental_events.parquet")
        m = json.loads((base / "events" / "manifest.json").read_bytes())
        m["files"]["fundamental_events.parquet"].update(
            bytes=(base / "events" / "fundamental_events.parquet").stat().st_size,
            sha256=sha(base / "events" / "fundamental_events.parquet"))
        (base / "events" / "manifest.json").write_text(json.dumps(m), encoding="utf-8")
        events_sha = sha(base / "events" / "manifest.json")
        self.produce("variants-out", ["be", "grp_sic2"], identity_bridge=base / "bridge",
                     identity_bridge_sha256=bridge_sha, fund_events=base / "events", fund_events_sha256=events_sha)
        for name in ("be", "grp_sic2"):
            np.testing.assert_array_equal(self.fx.field("variants-out", name), self.fx.field("issuer", name), name)
        # Events with no row for any linked CIK: every cell NaN, nothing refused.
        lone_sha = write_events(base / "lone", [x for x in EVENTS if x[0] == 7777],
                                [(7777, at(2024, 1, 1), 1000, "0000007777-24-000000")])
        self.produce("lone-out", ["be", "grp_ff12"], fund_events=base / "lone", fund_events_sha256=lone_sha)
        for name in ("be", "grp_ff12"):
            self.assertTrue(np.all(np.isnan(self.fx.field("lone-out", name))), name)

    def test_library_v4_field_names_are_producible(self):
        # atx-impl/strategies/fund_industry_ic_v4.json (T24, pool-7 3bb3fbf3) declares these non-base fields.
        declared = ["mkt_ret", "si_shares", "si_dtc", "iv_atm_21d", "earn_recent", "shares_out", "be", "at", "at_lag4",
                    "lt", "che", "debt", "sale_ttm", "gp_ttm", "oi_ttm", "ni_ttm", "ni_q", "ni_q_lag4", "be_lag1q",
                    "cfo_ttm", "capx_ttm", "xrd_ttm", "dvc_ttm", "prstkc_ttm", "sstk_ttm", "txt_q", "txt_q_lag4",
                    "shrs_q", "shrs_q_lag4", "noa", "sue", "fscore", "me_company", "grp_ff12", "grp_ff49"]
        self.assertEqual([x for x in declared if x not in tool.ALL_FIELDS], [])
        self.assertIn("be_lag1q_lag4", tool.ISSUER_FIELDS)  # canonical dROE (controller note)

    def test_cli_issuer_subset(self):
        with contextlib.redirect_stdout(io.StringIO()):
            tool.main(["--role", str(self.fx.role), "--role-sha256", self.fx.role_sha,
                       "--output", str(self.fx.base / "cli-issuer"), "--fields", "grp_ff12,be",
                       "--identity-bridge", str(self.bridge), "--identity-bridge-sha256", self.bridge_sha,
                       "--fund-events", str(self.events), "--fund-events-sha256", self.events_sha,
                       "--fund-lag-sessions", "1"])
        m = json.loads((self.fx.base / "cli-issuer" / "manifest.json").read_bytes())
        self.assertEqual([f["name"] for f in m["fields"]], ["be", "grp_ff12"])
        for name in ("be", "grp_ff12"):
            np.testing.assert_array_equal(self.fx.field("cli-issuer", name), self.fx.field("issuer", name))


if __name__ == "__main__":
    unittest.main()
