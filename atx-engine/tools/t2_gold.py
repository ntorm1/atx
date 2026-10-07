"""Test support for P9 lane T2, rulings T2-GOLD and T2-GOLD-4 (no tool imports this module).

``superseded_window`` binds the superseded research window research-seal-v1 (seal 2025-01-01) for tests whose
expected hashes were produced under it and are never edited (rule 4): test_linked_operating_v2.py,
test_linked_operating_v3.py and test_lo1_delisting.py (T2-GOLD, through conftest.py's module-scoped fixture) and
GOLDEN_ISSUER_FILES of test_prepare_research_fields_sic.py (T2-GOLD-4). The tools copy the window at import, so the
tools that do are reloaded under it; on exit the window bound before is bound again and each tool's namespace gets back
the exact objects it held before.

Below it is the FROZEN synthetic base fixture that GOLDEN_ISSUER_FILES pins (T2-GOLD-4): verbatim from
test_prepare_research_fields.py at 1239a5ff, before T2 re-dated that module's fixture to 2023 (lines 18-246: constants,
writers and Fixture; lines 982-1101: the issuer rows and writers). Its dates (an Oct-Dec 2024 role, sealed rows in
2025) are those of research-seal-v1, so it is built only inside ``superseded_window``. Do not edit it: any change moves
the pinned bytes. ``issuer_plain`` is the pinned build: the ISSUER_RUN recipe without --sic-events.
"""
import contextlib
import datetime as dt
import hashlib
import importlib
import io
import json
import math
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import prepare_research_fields as tool
import research_window

# The tools that copy the window at import and that the T2-GOLD tests run (prepare_recent_research imports
# prepare_research_fields inside restrict_role).
WINDOW_COPYING_TOOLS = ("prepare_research_fields", "prepare_recent_research")


@contextlib.contextmanager
def superseded_window():
    """research-seal-v1 for the duration of the block (T2-GOLD, T2-GOLD-4)."""
    modules = [importlib.import_module(name) for name in WINDOW_COPYING_TOOLS]
    namespaces = [dict(vars(m)) for m in modules]
    window = {key: getattr(research_window, key) for key in research_window.current()}
    research_window.bind(research_window.superseded())
    try:
        for m in modules:
            importlib.reload(m)
        yield
    finally:
        research_window.bind(window)
        for m, before in zip(modules, namespaces):
            ns = vars(m)
            for key in set(ns) - set(before):
                del ns[key]
            ns.update(before)


# ---- FROZEN: test_prepare_research_fields.py at 1239a5ff, lines 18-246 (verbatim) ------------------------------------
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

# ---- FROZEN: test_prepare_research_fields.py at 1239a5ff, lines 982-1101 (verbatim) ----------------------------------
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


# ---- the pinned build (T2-GOLD-4) ------------------------------------------------------------------------------------
ISSUER_RUN = list(tool.DEFAULT_FIELDS) + list(tool.ISSUER_FIELDS)


def issuer_plain(root: Path) -> dict:
    """The manifest of test_prepare_research_fields_sic.py's ``plain`` build on the frozen fixture: ISSUER_RUN with
    the identity bridge and fundamental events, no --sic-events. Call it inside ``superseded_window``."""
    if research_window.SEAL_DATE != research_window.superseded()["SEAL_DATE"]:
        raise RuntimeError("the frozen fixture is dated under research-seal-v1: build it inside superseded_window()")
    fx = Fixture(root)
    bridge, events = root / "bridge", root / "events"
    bridge_sha, events_sha = write_bridge(bridge), write_events(events)
    return fx.run("plain", fields=ISSUER_RUN, identity_bridge=bridge, identity_bridge_sha256=bridge_sha,
                  fund_events=events, fund_events_sha256=events_sha)
