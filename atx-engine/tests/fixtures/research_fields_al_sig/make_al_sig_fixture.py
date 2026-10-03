"""Synthetic fixture of the P9 lane AL-SIG field builders (gia_13f, russell_recon) and an independent oracle.

Writes a synthetic research role, an atx-db alpha-panel ``thirteenf/`` stage and an ``indexes/`` stage (parquet files
with the stage column types of research_fields_holdings.py's own test stages, each pinned by its manifest), then
computes both fields with this file's numpy oracle of the registered definitions (gia_13f_field.hpp, russell_recon_field.hpp),
reusing the house's Python 13F rules where they exist (research_fields_holdings.effective_filings, group_median,
prev_marks, anchor_quarters; research_fields_mgr13f.average_ranks). The C++ gtest ``ResearchFieldsAlSigFixture.*``
builds both fields from these files through the committed field registry and compares every cell with the oracle (NaN
pattern exact, values within 1e-12) and the reader counts exactly.

  python make_al_sig_fixture.py [--out DIR]     (default: this directory)

Layout under DIR:
  role/        atx.recent-research-role/v1 subset: manifest.json, sessions.i64, ids.u64, member.u8 (sessions are the
               weekdays of 2021, inside TRAIN)
  thirteenf/   manifest.json (atx.alpha-panel.thirteenf/v1), filings.parquet, filing_checks.parquet,
               cusip_map_pit.parquet, parts/source=YYYYqN/holdings.parquet; two poisoned parts (source=2023q4 and
               source=2024q1) are listed with a wrong SHA-256: opening either fails, which the sealed-part probe uses
  indexes/     manifest.json (atx.alpha-panel.indexes/v1), constituents.parquet (r1000, r2000, r3000, sp500 rows; one
               reconstitution past the seal)
  expected/    gia_13f.f64, russell_recon.f64 (the oracle's date-major payloads) and counts.json (reader counts)

Everything is synthetic and seeded; nothing real is read. Nothing here is a return, IC or NAV statistic of real data.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parents[2] / "tools"          # atx-engine/tools
sys.path.insert(0, str(TOOLS))
import research_fields_holdings as hold  # noqa: E402  (the house 13F rules)
import research_fields_mgr13f as mgr  # noqa: E402

UTC = dt.timezone.utc
DAY_NS = 86_400_000_000_000
SEAL_NS = hold.SEAL_NS
FIRST, LAST = dt.date(2021, 1, 4), dt.date(2021, 12, 31)
IDS = [101, 102, 103, 104, 105, 106, 107, 108, 110]          # role axis (110: no 13F / Russell row)
SIDS = [101, 102, 103, 104, 105, 106, 107, 108, 109]         # 109: off the role axis
MANAGERS = [1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008]
NOTICE_FILER = 1999
SPLIT_SID, SPLIT_QUARTER = 104, dt.date(2019, 12, 31)        # a 2:1 split seen by every holder
# GiaParams' defaults (gia_13f_field.hpp)
SKILL_Q, SKILL_MIN, MIN_HOLDERS, MIN_MANAGERS, MIN_COVERED, MAX_RATIO = 8, 6, 5, 3, 0.5, 10.0
STALE = 150


def sha(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def day_of(d: dt.date) -> int:
    return (d - dt.date(1970, 1, 1)).days


def instant(d: dt.date, hours: int = 0) -> dt.datetime:
    return dt.datetime(d.year, d.month, d.day, tzinfo=UTC) + dt.timedelta(hours=hours)


def ns_of(t: dt.datetime) -> int:
    return day_of(t.date()) * DAY_NS + (t.hour * 3600 + t.minute * 60 + t.second) * 1_000_000_000


def part_of(d: dt.date) -> str:
    return f"{d.year}q{(d.month - 1) // 3 + 1}"


def quarter_ends(first: dt.date, last: dt.date) -> list:
    out, q = [], hold.quarter_end(first)
    while q <= last:
        out.append(q)
        q = hold.quarter_end(q + dt.timedelta(days=1))
    return out


def sessions() -> list:
    out, d = [], FIRST
    while d <= LAST:
        if d.weekday() < 5:
            out.append(d)
        d += dt.timedelta(days=1)
    return out


# ---------------------------------------------------------------------------------------------------------- 13F data

STAGE_QUARTERS = quarter_ends(dt.date(2018, 6, 30), dt.date(2021, 9, 30))   # 2018-06-30 is outside the needed list


def market():
    """Seeded quarter-end prices per (quarter, security) and positions per (manager, quarter, security)."""
    rng = np.random.default_rng(20261003)
    nq, ns = len(STAGE_QUARTERS), len(SIDS)
    steps = rng.normal(0.0, 0.12, size=(nq, ns))
    price = 40.0 * np.exp(np.cumsum(steps, axis=0)) * (1.0 + np.arange(ns) / 10.0)
    split = STAGE_QUARTERS.index(SPLIT_QUARTER)
    price[split:, SIDS.index(SPLIT_SID)] /= 2.0
    shares = {}
    for k, cik in enumerate(MANAGERS):
        held = [s for i, s in enumerate(SIDS) if i not in (k % ns, (k + 3) % ns)]
        base = rng.uniform(0.5, 1.5, size=ns) * 1000.0 * (k + 1)
        for qi, q in enumerate(STAGE_QUARTERS):
            tilt = rng.uniform(0.6, 1.4, size=ns)
            for i, s in enumerate(SIDS):
                if s in held:
                    n = float(np.round(base[i] * tilt[i]))
                    if s == SPLIT_SID and qi >= split:
                        n *= 2.0
                    shares[(cik, q, s)] = n
    return price, shares


def filings_and_rows(price, shares):
    """Filings (dicts), holdings rows per accession and unit factors, with the rule cases planted."""
    filings, rows, unit = [], {}, {}
    qa, qb, qc, qd = dt.date(2019, 6, 30), dt.date(2020, 3, 31), dt.date(2020, 6, 30), dt.date(2020, 9, 30)

    def file(cik, q, fdate, form, amend, holdings, tag="0"):
        acc = f"{cik:010d}-{q:%y%m%d}-{tag}"
        filings.append({"accession": acc, "filer_cik": str(cik), "period_q": q, "filing_date": fdate,
                        "submission_type": form, "amendment_type": amend, "available_at": instant(fdate, 46),
                        "deadline": q + dt.timedelta(days=45), "source_period": part_of(fdate)})
        rows[acc] = holdings
        return acc

    for k, cik in enumerate(MANAGERS):
        for qi, q in enumerate(STAGE_QUARTERS):
            book = [(f"C{s}", shares[(cik, q, s)], "SH", None, shares[(cik, q, s)] * price[qi, SIDS.index(s)])
                    for s in SIDS if (cik, q, s) in shares]
            fdate = q + dt.timedelta(days=20 + k)
            if cik == 1003 and q == qa:      # a wrong original, restated before the deadline: only the restatement
                file(cik, q, fdate, "13F-HR", None, [(c, n * 99, t, p, v * 99) for c, n, t, p, v in book])
                file(cik, q, fdate + dt.timedelta(days=5), "13F-HR/A", "RESTATEMENT", book, "r")
                continue
            if cik == 1006 and q == qc:      # a restatement after the deadline: ignored
                file(cik, q, fdate, "13F-HR", None, book)
                file(cik, q, q + dt.timedelta(days=60), "13F-HR/A", "RESTATEMENT",
                     [(c, n * 7, t, p, v * 7) for c, n, t, p, v in book], "late")
                continue
            if cik == 1007 and q == qd:      # values reported x1000: unit factor .001
                acc = file(cik, q, fdate, "13F-HR", None, [(c, n, t, p, v * 1000.0) for c, n, t, p, v in book])
                unit[acc] = 0.001
                continue
            extra = []
            if cik == 1002 and q == qb:      # rows every screen drops, and an unmapped and an off-axis CUSIP
                extra = [("C101", 50.0, "SH", "CALL", 500.0), ("C101", 50.0, "PRN", None, 500.0),
                         ("C101", 0.0, "SH", None, 0.0), ("Z00000000", 10.0, "SH", None, 100.0),
                         ("C102", 10.0, "SH", None, 10.0 * price[qi, SIDS.index(102)] * 50.0)]   # price outlier
            file(cik, q, fdate, "13F-HR", None, book + extra)
            if cik == 1004 and q == qb:      # a NEW HOLDINGS amendment after the base: both effective
                file(cik, q, fdate + dt.timedelta(days=3), "13F-HR/A", "NEW HOLDINGS",
                     [("C109", 500.0, "SH", None, 500.0 * price[qi, SIDS.index(109)])], "n")
            if cik == 1005 and q == qb:      # an untyped 13F-HR/A: not used
                file(cik, q, fdate + dt.timedelta(days=4), "13F-HR/A", None, [("C101", 1e6, "SH", None, 1e9)], "x")
        # a notice late inside the deadline window moves V but holds nothing
    for q in STAGE_QUARTERS:
        file(NOTICE_FILER, q, q + dt.timedelta(days=44), "13F-NT", None, [])
    unit_rows = [(acc, f) for acc, f in unit.items()]
    nan_acc = f"{1001:010d}-{dt.date(2020, 12, 31):%y%m%d}-0"     # an effective filing of a needed quarter
    unit_rows += [(nan_acc, float("nan")), (nan_acc, 7.0)]       # the first row wins, and NaN reads as 1
    return filings, rows, unit_rows


def write_stage(root: Path, schema: str, rules: dict, tables: dict, poisoned=()) -> str:
    files = {}
    for rel, table in tables.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        pq.write_table(table, path)
        files[rel] = {"bytes": path.stat().st_size, "sha256": "0" * 64 if rel in poisoned else sha(path.read_bytes())}
    manifest = {"schema": schema, "status": "complete", **rules, "files": files}
    blob = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    (root / "manifest.json").write_bytes(blob)
    return sha(blob)


def ts(values):
    return pa.array(values, pa.timestamp("us", tz="UTC"))


def holdings_table(rows: list) -> pa.Table:
    return pa.table({"accession": [r[0] for r in rows], "cusip": [r[1] for r in rows],
                     "shares": pa.array([float(r[2]) for r in rows], pa.float64()),
                     "sshprnamt_type": [r[3] for r in rows], "put_call": pa.array([r[4] for r in rows], pa.string()),
                     "value_usd": pa.array([float(r[5]) for r in rows], pa.float64())})


def write_thirteenf(root: Path, filings, rows, unit_rows) -> str:
    f = filings
    tables = {"filings.parquet": pa.table({
        "accession": [x["accession"] for x in f], "filer_cik": [x["filer_cik"] for x in f],
        "period_q": pa.array([x["period_q"] for x in f], pa.date32()),
        "filing_date": pa.array([x["filing_date"] for x in f], pa.date32()),
        "submission_type": [x["submission_type"] for x in f],
        "amendment_type": pa.array([x["amendment_type"] for x in f], pa.string()),
        "available_at": ts([x["available_at"] for x in f]),
        "deadline": pa.array([x["deadline"] for x in f], pa.date32()),
        "source_period": [x["source_period"] for x in f]})}
    parts: dict = {}
    for x in f:
        for c, n, t, p, v in rows[x["accession"]]:
            parts.setdefault(x["source_period"], []).append((x["accession"], c, n, t, p, v))
    for part, prow in sorted(parts.items()):
        tables[f"parts/source={part}/holdings.parquet"] = holdings_table(prow)
    poison = [("poison-acc", "C101", 1.0, "SH", None, 1.0)]
    for part in ("2023q4", "2024q1"):
        tables[f"parts/source={part}/holdings.parquet"] = holdings_table(poison)
    tables["filing_checks.parquet"] = pa.table({"accession": [a for a, _ in unit_rows],
                                                "unit_factor": pa.array([u for _, u in unit_rows], pa.float64())})
    cm = [(q, f"C{s}", s) for q in STAGE_QUARTERS for s in SIDS] + [(q, "C101", 999) for q in STAGE_QUARTERS]
    tables["cusip_map_pit.parquet"] = pa.table({"period_q": pa.array([x[0] for x in cm], pa.date32()),
                                                "cusip": [x[1] for x in cm],
                                                "security_id": pa.array([x[2] for x in cm], pa.int64())})
    rules = {k: v for k, v in hold.STAGES["thirteenf"].items() if k != "schema"}
    poisoned = ("parts/source=2023q4/holdings.parquet", "parts/source=2024q1/holdings.parquet")
    return write_stage(root, hold.STAGES["thirteenf"]["schema"], rules, tables, poisoned)


# ------------------------------------------------------------------------------------------------- Russell proxy data

RECONS = [  # (rank date, recon date, {sid: index}) ; index "both" = a conflict row in r1000 and r2000
    (dt.date(2019, 5, 10), dt.date(2019, 6, 28), {101: "r1000", 102: "r2000", 103: "r1000", 104: "r2000", 106: "r2000",
                                                  107: "r1000"}),
    (dt.date(2020, 5, 8), dt.date(2020, 6, 26), {101: "r1000", 102: "r2000", 103: "r1000", 104: "r2000", 106: "r2000",
                                                 107: "r2000"}),
    (dt.date(2021, 5, 7), dt.date(2021, 6, 25), {101: "r2000", 102: "r1000", 103: "r1000", 104: "r2000", 105: "r2000",
                                                 107: "both"}),
    (dt.date(2024, 4, 30), dt.date(2024, 6, 28), {101: "r1000", 102: "r2000"}),     # sealed: dropped and counted
]


def russell_rows() -> list:
    out = []   # (index_id, security_id, rank, recon, available)
    for rank, recon, members in RECONS:
        for sid, idx in members.items():
            for index_id in (("r1000", "r2000") if idx == "both" else (idx,)):
                out.append((index_id, sid, rank, recon, instant(rank, 22)))
            out.append(("r3000", sid, rank, recon, instant(rank, 22)))
        out.append(("sp500", 103, rank, recon, instant(rank, 22)))
    out.append(("r2000", None, RECONS[2][0], RECONS[2][1], instant(RECONS[2][0], 22)))   # null id: unusable
    return out


def write_indexes(root: Path) -> str:
    rows = russell_rows()
    table = pa.table({"index_id": [r[0] for r in rows], "security_id": pa.array([r[1] for r in rows], pa.int64()),
                      "cik": pa.array([None] * len(rows), pa.int64()),
                      "effective_from": pa.array([r[3] + dt.timedelta(days=3) for r in rows], pa.date32()),
                      "effective_to": pa.array([None] * len(rows), pa.date32()),
                      "weight": pa.array([0.001] * len(rows), pa.float64()),
                      "company_rank": pa.array(list(range(1, len(rows) + 1)), pa.int64()),
                      "rank_date": pa.array([r[2] for r in rows], pa.date32()),
                      "recon_date": pa.array([r[3] for r in rows], pa.date32()),
                      "basis": ["rank"] * len(rows), "available_at": ts([r[4] for r in rows])})
    return write_stage(root, "atx.alpha-panel.indexes/v1", {"rule": "russell-proxy-v1"},
                       {"constituents.parquet": table})


# ------------------------------------------------------------------------------------------------------------- role

def member_matrix(days: list) -> np.ndarray:
    m = np.ones((len(days), len(IDS)), dtype=np.uint8)
    m[:20, IDS.index(110)] = 0
    m[100:110, IDS.index(105)] = 0
    return m


def write_role(root: Path, days: list) -> str:
    root.mkdir(parents=True, exist_ok=True)
    blobs = {"sessions.i64": np.array([day_of(d) * DAY_NS for d in days], dtype="<i8").tobytes(),
             "ids.u64": np.array(IDS, dtype="<u8").tobytes(), "member.u8": member_matrix(days).tobytes()}
    files = {}
    for name, blob in blobs.items():
        (root / name).write_bytes(blob)
        files[name] = {"bytes": len(blob), "sha256": sha(blob)}
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete", "dates": len(days),
                "instruments": len(IDS), "instrument_namespace": "spiderrock.securityID", "score_begin": 0,
                "score_end": len(days), "source_sha256": "0" * 64, "files": files}
    blob = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    (root / "manifest.json").write_bytes(blob)
    return sha(blob)


# ---------------------------------------------------------------------------------------------------- the oracle

def oracle_quarters(days: list) -> list:
    q0 = hold.quarter_end(days[0] - dt.timedelta(days=STALE))
    first = q0
    for _ in range(SKILL_Q):
        first = hold.prev_quarter_end(first)
    out, q = [], first
    while q < days[-1]:
        out.append(q)
        q = hold.quarter_end(q + dt.timedelta(days=1))
    return out


def oracle_positions(rows: list):
    """rows: (filer, sid, shares, value) of one quarter -> the quarter positions (thirteenf.hpp)."""
    rows = [r for r in rows if r[1] >= 0]
    rows_mapped = len(rows)
    priced = [(r[1], r[3] / r[2]) for r in rows if r[3] > 0]
    us, med = hold.group_median(np.array([p[0] for p in priced], dtype=np.int64),
                                np.array([p[1] for p in priced])) if priced else (np.zeros(0, np.int64), np.zeros(0))
    ref = dict(zip(us.tolist(), med.tolist()))
    kept, outliers = [], 0
    for r in rows:
        if r[3] > 0:
            ratio = (r[3] / r[2]) / ref[r[1]]
            if ratio > 10.0 or ratio < 0.1:
                outliers += 1
                continue
        kept.append(r)
    pairs: dict = {}
    for f, s, n, v in kept:   # dict insertion keeps row order inside a pair
        a = pairs.setdefault((f, s), [0.0, 0.0])
        a[0] += n
        a[1] += v
    keys = sorted(pairs)
    filer_value: dict = {}
    for f, s in keys:
        filer_value[f] = filer_value.get(f, 0.0) + pairs[(f, s)][1]
    kp = [(r[1], r[3] / r[2]) for r in kept if r[3] > 0]
    ps, pp = hold.group_median(np.array([p[0] for p in kp], dtype=np.int64),
                               np.array([p[1] for p in kp])) if kp else (np.zeros(0, np.int64), np.zeros(0))
    return {"pairs": {k: pairs[k] for k in keys}, "filer_value": filer_value,
            "price": dict(zip(ps.tolist(), pp.tolist())), "rows_mapped": rows_mapped, "outliers": outliers}


def oracle_basis(prev, cur, sid) -> float:
    both = [k for k in prev["pairs"] if k in cur["pairs"] and k[1] == sid]
    ratios = sorted(cur["pairs"][k][0] / prev["pairs"][k][0] for k in both)
    if len(ratios) < 5:
        return 1.0
    s = (ratios[(len(ratios) - 1) // 2] + ratios[len(ratios) // 2]) / 2.0
    pc, pp = cur["price"].get(sid, np.nan), prev["price"].get(sid, np.nan)
    with np.errstate(invalid="ignore", divide="ignore"):
        offset = abs(np.log(s * pc / pp)) <= np.log(1.5)
    return s if abs(np.log(s)) > np.log(1.2) and offset else 1.0


def oracle_returns(prev, cur):
    r = {}
    for sid, before in prev["price"].items():
        after = cur["price"].get(sid, np.nan)
        if not (before > 0 and after > 0):
            continue
        ratio = oracle_basis(prev, cur, sid) * after / before
        if np.isfinite(ratio) and 1.0 / MAX_RATIO <= ratio <= MAX_RATIO:
            r[sid] = ratio - 1.0
    filer, s_all, c_all, t_all = {}, 0.0, 0.0, 0.0
    for f in sorted(prev["filer_value"]):
        s = c = 0.0
        for (pf, sid), (_, v) in prev["pairs"].items():
            if pf == f and v > 0 and sid in r:
                s += v * r[sid]
                c += v
        total = prev["filer_value"][f]
        if c > 0 and c >= MIN_COVERED * total:
            filer[f] = s / c
        s_all, c_all, t_all = s_all + s, c_all + c, t_all + total
    agg = s_all / c_all if c_all > 0 and c_all >= MIN_COVERED * t_all else np.nan
    return filer, agg


def oracle_gia(days, filings, rows, unit_rows):
    quarters = oracle_quarters(days)
    qd = {q: i for i, q in enumerate(quarters)}
    unit: dict = {}
    for acc, u in unit_rows:
        unit.setdefault(acc, u)
    cmap: dict = {}
    for q in quarters:
        for s in SIDS:
            cmap.setdefault((q, f"C{s}"), s)
    f = [x for x in filings if x["period_q"] in qd]
    counts = {"filings_after_deadline_ignored": sum(1 for x in f if x["filing_date"] > x["deadline"]),
              "effective_filings": 0, "holdings_rows_kept": 0, "per_quarter": []}
    inq = [x for x in f if x["filing_date"] <= x["deadline"]]
    sel = [x for x in inq if x["submission_type"] in hold.F13_FORMS]
    eff = hold.effective_filings([day_of(x["period_q"]) for x in sel], [int(x["filer_cik"]) for x in sel],
                                 [day_of(x["filing_date"]) for x in sel], [x["accession"] for x in sel],
                                 [x["submission_type"] for x in sel], [x["amendment_type"] or "" for x in sel])
    effective = [x for x, e in zip(sel, eff) if e]
    counts["effective_filings"] = len(effective)
    n, nq = len(IDS), len(quarters)
    value, has, vstar = np.full((nq, n), np.nan), np.zeros((nq, n), dtype=bool), np.full(nq, hold.NEVER)
    excess: dict = {}
    prev, running = None, hold.BEFORE_ALL
    for qi, q in enumerate(quarters):
        mine = [x for x in inq if x["period_q"] == q]
        cur = None
        if mine:
            vis = max(ns_of(x["available_at"]) for x in mine)
            prow = []
            for x in sorted((x for x in effective if x["period_q"] == q), key=lambda x: x["accession"]):
                factor = unit.get(x["accession"], 1.0)
                factor = factor if np.isfinite(factor) and factor > 0 else 1.0
                for c, nsh, t, pc, v in rows[x["accession"]]:
                    if t == "SH" and not pc and np.isfinite(nsh) and nsh > 0 and np.isfinite(v) and v >= 0:
                        prow.append((int(x["filer_cik"]), cmap.get((q, c), -1), nsh, v * factor))
            counts["holdings_rows_kept"] += len(prow)
            cur = oracle_positions(prow)
        rec = {"quarter": q.isoformat(), "positions": cur is not None, "filers_with_return": 0, "managers_ranked": 0,
               "securities_scored": 0, "rows_mapped": cur["rows_mapped"] if cur else 0,
               "rows_price_outlier": cur["outliers"] if cur else 0}
        if cur is not None and prev is not None:
            filer, agg = oracle_returns(prev, cur)
            if np.isfinite(agg):
                for k, rk in filer.items():
                    excess.setdefault(k, []).append((qi, rk - agg))
                    rec["filers_with_return"] += 1
        if cur is not None:
            running = max(running, vis)
            vstar[qi] = running
            filers = sorted(cur["filer_value"])
            skill = {}
            for k in filers:
                e = [x for j, x in excess.get(k, []) if qi - SKILL_Q < j <= qi]
                if len(e) >= SKILL_MIN:
                    skill[k] = sum(e) / len(e)
            u = {}
            if len(skill) >= MIN_MANAGERS:
                ks = sorted(skill)
                ranks = mgr.average_ranks(np.array([skill[k] for k in ks]))
                m = len(ks)
                u = {k: (r - (m + 1) / 2.0) / m for k, r in zip(ks, ranks)}
                rec["managers_ranked"] = m
            acc: dict = {}
            for (k, sid), (_, v) in cur["pairs"].items():
                a = acc.setdefault(sid, [0.0, 0.0, 0])
                total = cur["filer_value"][k]
                if k in u and total > 0 and v / total > 0:
                    w = v / total
                    a[0] += w * u[k]
                    a[1] += w
                    a[2] += 1
            for sid, (wu, w, h) in acc.items():
                if sid in IDS:
                    j = IDS.index(sid)
                    has[qi, j] = True
                    if h >= MIN_HOLDERS and w > 0:
                        value[qi, j] = wu / w
                        rec["securities_scored"] += 1
        counts["per_quarter"].append(rec)
        prev = cur
    dd = np.array([day_of(d) for d in days], dtype=np.int64)
    pm = hold.prev_marks(dd)
    pdays = np.array([day_of(q) for q in quarters], dtype=np.int64)
    out = np.full((len(days), n), np.nan)
    for t in range(len(days)):
        anchor = hold.anchor_quarters(pdays, vstar, has, int(dd[t]), int(pm[t]))
        ok = anchor >= 0
        out[t, ok] = value[anchor[ok], np.flatnonzero(ok)]
    counts["quarters"] = [q.isoformat() for q in quarters]
    return out, counts


def oracle_russell(days):
    rows = russell_rows()
    counts = {"rows_total": len(rows), "rows_other_index": sum(1 for r in rows if r[0] not in ("r1000", "r2000")),
              "rows_unusable": sum(1 for r in rows if r[0] in ("r1000", "r2000") and r[1] is None)}
    usable = [r for r in rows if r[0] in ("r1000", "r2000") and r[1] is not None]
    counts["rows_sealed"] = sum(1 for r in usable if ns_of(r[4]) >= SEAL_NS)
    usable = [r for r in usable if ns_of(r[4]) < SEAL_NS]
    recons = sorted({r[3] for r in usable})
    snap = {R: {"r1000": {r[1] for r in usable if r[3] == R and r[0] == "r1000"},
                "r2000": {r[1] for r in usable if r[3] == R and r[0] == "r2000"},
                "vis": max(ns_of(r[4]) for r in usable if r[3] == R)} for R in recons}
    moves = []
    for a, b in zip(recons, recons[1:]):
        p, c = snap[a], snap[b]
        v = np.full(len(IDS), np.nan)
        for j, sid in enumerate(IDS):
            w1, w2, i1, i2 = sid in p["r1000"], sid in p["r2000"], sid in c["r1000"], sid in c["r2000"]
            if (w1 and w2) or (i1 and i2):
                continue
            v[j] = 1.0 if (w1 and i2) else -1.0 if (w2 and i1) else 0.0 if ((w1 and i1) or (w2 and i2)) else np.nan
        moves.append((day_of(b), max(p["vis"], c["vis"]), v))
    counts["reconstitutions"] = len(recons)
    dd = np.array([day_of(d) for d in days], dtype=np.int64)
    pm = hold.prev_marks(dd)
    out = np.full((len(days), len(IDS)), np.nan)
    for t in range(len(days)):
        vis = [m for m in moves if m[1] < pm[t]]
        if vis:
            m = max(vis, key=lambda m: m[0])
            if dd[t] < m[0]:
                out[t] = m[2]
    return out, counts


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--out", type=Path, default=HERE)
    a = ap.parse_args(argv)
    out = a.out
    days = sessions()
    price, shares = market()
    filings, rows, unit_rows = filings_and_rows(price, shares)
    pins = {"role": write_role(out / "role", days), "thirteenf": write_thirteenf(out / "thirteenf", filings, rows,
                                                                                    unit_rows),
            "indexes": write_indexes(out / "indexes")}
    gia, gia_counts = oracle_gia(days, filings, rows, unit_rows)
    rus, rus_counts = oracle_russell(days)
    (out / "expected").mkdir(parents=True, exist_ok=True)
    (out / "expected" / "gia_13f.f64").write_bytes(gia.astype("<f8").tobytes())
    (out / "expected" / "russell_recon.f64").write_bytes(rus.astype("<f8").tobytes())
    counts = {"pins": pins, "gia_13f": gia_counts, "russell_recon": rus_counts,
              "finite": {"gia_13f": int(np.isfinite(gia).sum()), "russell_recon": int(np.isfinite(rus).sum())}}
    (out / "expected" / "counts.json").write_text(json.dumps(counts, indent=2, sort_keys=True) + "\n", encoding="utf-8",
                                                  newline="\n")
    print(json.dumps(counts["finite"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
