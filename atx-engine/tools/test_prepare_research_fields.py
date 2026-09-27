"""Small synthetic postimplementation checks; no real archive/warehouse/lake access."""
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
    rows = {k: [] for k in ("tradingDate", "securityID", "close", "shares", "earnFlag", "cumulReturnFactor",
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
            for k, x in (("tradingDate", d), ("securityID", sid), ("close", 10.0), ("shares", shares),
                         ("earnFlag", flag), ("cumulReturnFactor", crf), ("atmCenI_21d", v),
                         ("atmCenI_63d", None if v is None else np.float32(v + np.float32(0.05))),
                         ("atmCenI_126d", None if v is None else np.float32(v + np.float32(0.1)))):
                rows[k].append(x)
    # One positive duplicate key (all its rows quarantined) and one weekend row (off calendar).
    t, sid = DUP
    extra = [(SESSIONS[t], sid, 0.9), (dt.date(2024, 10, 5), 101, 0.7)]
    for d, sid, v in extra:
        for k, x in (("tradingDate", d), ("securityID", sid), ("close", 10.0), ("shares", 2000),
                     ("earnFlag", "N"), ("cumulReturnFactor", 1.0), ("atmCenI_21d", np.float32(v)),
                     ("atmCenI_63d", np.float32(v)), ("atmCenI_126d", np.float32(v))):
            rows[k].append(x)
    order = np.random.default_rng(7).permutation(len(rows["securityID"]))  # dates mixed in every group
    table = pa.table({
        "tradingDate": pa.array([rows["tradingDate"][i] for i in order], pa.date32()),
        "securityID": pa.array([rows["securityID"][i] for i in order], pa.int64()),
        "close": pa.array([rows["close"][i] for i in order], pa.float32()),
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


def write_finra(root):
    (root / "asof").mkdir(parents=True)
    dates = sorted({r[1] for r in FINRA_SHARES + FINRA_DTC} | {"2021-06-09"})
    lines = ["settlement_date,due_date,dissemination_date,source,third_column_label,in_download,source_url"]
    lines.append("2021-05-28,2021-06-02,2021-06-09,official,Publication Date,True,x")
    lines += [f"{(dt.date.fromisoformat(d) - dt.timedelta(days=9)).isoformat()},x,{d},official,Publication Date,True,x"
              for d in dates if d != "2021-06-09"]
    (root / "dissemination_schedule.csv").write_text("\n".join(lines) + "\n", encoding="utf-8")
    outputs = {}
    for name, rows in (("si_shares", FINRA_SHARES), ("si_dtc", FINRA_DTC)):
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
             "present.u8": present.tobytes()}
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
            self.assertEqual("plausibility" in entry, name in tenors)

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


if __name__ == "__main__":
    unittest.main()
