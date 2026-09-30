"""SEC-derived fields (research_fields_sec.py, platform v7 W5a): synthetic stages, no real archive access.

Every field is re-derived cell by cell by a brute-force oracle written from the field definitions (its own hand-listed
NYSE calendar, datetime visibility tests, set-based distinct counts and the Cohen-Malloy-Pomorski classification by
month sets), so the module's vectorised streaming (Latest pointers, [enter, leave) event windows, merged owner
intervals, bit-mask classification) is checked against an independent implementation. Planted cases: filings accepted
22:30 UTC on d-1 (not usable at d) and 21:59 UTC on d-1 (usable at d); a late earnings announcement (overdue, then
consumed); an early year-ago expectation consumed by an announcement; the +91 fallback; the 63-session overdue cap and
the 200-day staleness; routine / opportunistic / unclassified insiders; excluded amendments, Form 5, derivative, 10%
owner, zero-share and direction-mismatch rows; a late Form 4 outside the window; Section 16 and 8-K presence expiry; a
link that starts inside the role; shares_out NaN. The byte-identity test runs the full legacy + issuer + sv recipe with
and without the SEC fields.
"""
import contextlib
import datetime as dt
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
import research_fields_sec as sec
import research_window as rw
import test_prepare_research_fields as base
import test_prepare_research_fields_sv as svt

SESSIONS, IDS = base.SESSIONS, base.IDS
UTC = dt.timezone.utc
D = dt.date.fromisoformat
at, mark = base.at, base.mark
LONG_AGO = base.LONG_AGO
SEAL = at(2025, 1, 1)

# Hand-listed NYSE full-day closures 2023-2026 (independent of sec.nyse_holidays).
HOLIDAYS = {D(x) for x in (
    "2023-01-02", "2023-01-16", "2023-02-20", "2023-04-07", "2023-05-29", "2023-06-19", "2023-07-04", "2023-09-04",
    "2023-11-23", "2023-12-25", "2024-01-01", "2024-01-15", "2024-02-19", "2024-03-29", "2024-05-27", "2024-06-19",
    "2024-07-04", "2024-09-02", "2024-11-28", "2024-12-25", "2025-01-01", "2025-01-09", "2025-01-20", "2025-02-17",
    "2025-04-18", "2025-05-26", "2025-06-19", "2025-07-04", "2025-09-01", "2025-11-27", "2025-12-25",
    "2026-01-01", "2026-01-19", "2026-02-16", "2026-04-03", "2026-05-25", "2026-06-19", "2026-07-03",
    "2026-09-07", "2026-11-26", "2026-12-25")}
_ALL = [D("2023-01-02") + dt.timedelta(days=i) for i in range(1460)]   # through 2026-12-31
# the role's own sessions inside the role (the fixture role keeps 2024-11-28 and 2024-12-25), the rule outside
CAL = sorted({d for d in _ALL if d.weekday() < 5 and d not in HOLIDAYS and not SESSIONS[0] <= d <= SESSIONS[-1]}
             | set(SESSIONS))
IDX = {d: i for i, d in enumerate(CAL)}


def prev(d):
    return CAL[IDX[d] - 1]


def usable(avail, d):
    """The SEC clock: available_at < 22:00 UTC of the session before d."""
    return avail < SEAL and avail < mark(prev(d))


def at_or_after(d):
    return next(i for i, s in enumerate(CAL) if s >= d)


def event_session(avail):
    return next(i for i, s in enumerate(CAL) if mark(s) > avail)


def first_session_on_or_after(d):
    return CAL[at_or_after(d)]


# (sr_id, cik, start, end_incl, available_at, primary, tier, basis): the SEC bridge (atx-db identity-bridge-v2-pit)
SEC_BRIDGE = [
    (101, 1001, D("2015-01-01"), None, LONG_AGO, "P", "strict", "reconstructed_high"),
    (303, 1001, D("2015-01-01"), None, LONG_AGO, "J", "strict", "reconstructed_high"),     # second class: NaN
    (202, 2002, D("2015-01-01"), None, LONG_AGO, "P", "strict", "reconstructed_high"),
    (404, 4004, D("2015-01-01"), D("2024-12-02"), LONG_AGO, "P", "strict", "reconstructed_high"),  # ends in the role
    (505, 5005, D("2024-11-01"), None, mark(D("2024-11-01")), "P", "name", "finra_name_match"),  # starts in the role
    (999, 9009, D("2015-01-01"), None, LONG_AGO, "P", "strict", "reconstructed_high"),     # off the role axis
]


def sec_link(sid, d):
    q = [(c, k) for s, c, start, end, avail, k, _, _ in SEC_BRIDGE
         if s == sid and start <= d and (end is None or d <= end) and avail <= mark(d)]
    return (q[0][0], q[0][1] == "P") if q else (None, False)


# ---- earnings calendar -------------------------------------------------------------------------------------------
# (cik, accession, available_at, timing, session_date, reaction_session, primary, expected_rule, expected_error_days)
def post(d):
    return first_session_on_or_after(d + dt.timedelta(days=1))


ANN = [
    (1001, "0000001001-23-000010", at(2023, 10, 24, 20, 30), "post_market", D("2023-10-24"), post(D("2023-10-24")),
     True, "yoy_364", 1),
    (1001, "0000001001-24-000001", at(2024, 1, 25, 12), "pre_market", D("2024-01-25"), D("2024-01-25"), True,
     "yoy_364", -2),
    (1001, "0000001001-24-000002", at(2024, 4, 23, 20, 15), "post_market", D("2024-04-23"), post(D("2024-04-23")),
     True, "yoy_364", 0),
    (1001, "0000001001-24-000003", at(2024, 7, 23, 20, 10), "intraday", D("2024-07-23"), D("2024-07-23"), True,
     "yoy_364", 3),
    # late (expected 10-22) and accepted 22:30 UTC on 10-29: not usable on 10-30, usable from 10-31
    (1001, "0000001001-24-000004", at(2024, 10, 29, 22, 30), "post_market", D("2024-10-29"), D("2024-10-30"), True,
     "yoy_364", 7),
    (1001, "0000001001-24-000005", at(2024, 11, 5, 13), "pre_market", D("2024-11-05"), D("2024-11-05"), False,
     None, None),                                                   # non-primary: ignored
    (1001, "0000001001-25-000001", at(2025, 1, 28, 21), "post_market", D("2025-01-28"), D("2025-01-29"), True,
     "yoy_364", 0),                                                 # sealed
    # young filer: no year-ago rows -> fallback +91; the second release at 21:59 UTC on 11-07 is usable on 11-08
    (2002, "0000002002-24-000001", at(2024, 8, 6, 11, 30), "pre_market", D("2024-08-06"), D("2024-08-06"), True,
     None, None),
    (2002, "0000002002-24-000002", at(2024, 11, 7, 21, 59), "post_market", D("2024-11-07"), D("2024-11-08"), True,
     "prev_primary_plus_91", 2),
    # missed quarter: overdue beyond 63 sessions, then stale after 200 days
    (4004, "0000004004-23-000001", at(2023, 7, 20, 20), "post_market", D("2023-07-20"), D("2023-07-21"), True,
     "yoy_364", 0),
    (4004, "0000004004-23-000002", at(2023, 10, 19, 20), "post_market", D("2023-10-19"), D("2023-10-20"), True,
     "yoy_364", 1),
    (4004, "0000004004-24-000001", at(2024, 5, 2, 20), "post_market", D("2024-05-02"), D("2024-05-03"), True,
     "yoy_364", 14),
    # early announcement: the year-ago expectation (2024-11-05) is consumed by the 2024-10-24 release
    (5005, "0000005005-23-000001", at(2023, 11, 7, 12), "pre_market", D("2023-11-07"), D("2023-11-07"), True,
     "yoy_364", 0),
    (5005, "0000005005-24-000001", at(2024, 2, 8, 12), "pre_market", D("2024-02-08"), D("2024-02-08"), True,
     "yoy_364", 0),
    (5005, "0000005005-24-000002", at(2024, 5, 9, 12), "pre_market", D("2024-05-09"), D("2024-05-09"), True,
     "yoy_364", 0),
    (5005, "0000005005-24-000003", at(2024, 8, 8, 12), "closed_day", D("2024-08-08"), D("2024-08-08"), True,
     "yoy_364", 0),
    (5005, "0000005005-24-000004", at(2024, 10, 24, 12), "pre_market", D("2024-10-24"), D("2024-10-24"), True,
     "yoy_364", -12),
    (7777, "0000007777-24-000001", at(2024, 10, 2, 12), "pre_market", D("2024-10-02"), D("2024-10-02"), True,
     "yoy_364", 0),                                                 # unlinked CIK
]
TIMING = {"pre_market": 0.0, "intraday": 1.0, "post_market": 2.0, "closed_day": 3.0}


def next_expected(sdate):
    return first_session_on_or_after(sdate + dt.timedelta(days=364))


def naive(x):
    return x.replace(tzinfo=None)


def write_stage(root, schema, tables):
    root.mkdir(parents=True)
    files = {}
    for rel, table in tables.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        pq.write_table(table, path)
        files[rel] = {"bytes": path.stat().st_size, "sha256": base.sha(path)}
    (root / "manifest.json").write_text(json.dumps({"schema": schema, "status": "complete", "files": files}),
                                        encoding="utf-8")
    return base.sha(root / "manifest.json")


def announcements_table(rows=ANN):
    c = list(zip(*rows))
    return pa.table({
        "cik": pa.array(c[0], pa.int64()), "accession": pa.array(c[1], pa.string()),
        "form": pa.array(["8-K"] * len(rows), pa.string()),
        "available_at": pa.array([naive(x) for x in c[2]], pa.timestamp("us")),
        "timing": pa.array(c[3], pa.string()), "session_date": pa.array(c[4], pa.date32()),
        "reaction_session": pa.array(c[5], pa.date32()), "is_primary": pa.array(c[6], pa.bool_()),
        "next_expected_date": pa.array([next_expected(s) if p else None for s, p in zip(c[4], c[6])], pa.date32()),
        "expected_rule": pa.array(c[7], pa.string()), "expected_error_days": pa.array(c[8], pa.int32())})


# ---- Form 4 --------------------------------------------------------------------------------------------------------
def tx(issuer, owner, txn, avail, code="P", shares=100.0, form="4", table="non_derivative", director=True,
       officer=False, direction=None, owners=1, ten=False):
    return {"issuer_cik": issuer, "owner_cik": owner, "form": form, "is_amendment": form.endswith("/A"),
            "table_type": table, "transaction_code": code,
            "acquired_disposed": direction or ("A" if code in ("P", "A", "M") else "D"), "shares": shares,
            "transaction_date": txn, "any_director": director, "any_officer": officer, "any_ten_percent_owner": ten,
            "n_reporting_owners": owners, "available_at": avail}


def day_after(d, hh=21):
    n = d + dt.timedelta(days=1)
    return at(n.year, n.month, n.day, hh)


TXS = [
    # history of CIK 1001 insiders for the 2024 classification
    *[tx(1001, 11, D(x), day_after(D(x)), "S", 100) for x in ("2021-03-10", "2022-03-15", "2023-03-14")],  # routine
    tx(1001, 12, D("2021-02-10"), day_after(D("2021-02-10")), "P", 50),                                   # opportunistic
    tx(1001, 12, D("2022-06-15"), day_after(D("2022-06-15")), "S", 60),
    tx(1001, 12, D("2023-09-12"), day_after(D("2023-09-12")), "P", 70),
    tx(1001, 13, D("2023-05-05"), day_after(D("2023-05-05")), "P", 10),                                   # unclassified
    tx(1001, 21, D("2021-01-05"), day_after(D("2021-01-05")), "P", 5, officer=True, director=False),
    tx(1001, 21, D("2022-01-05"), day_after(D("2022-01-05")), "P", 5, officer=True, director=False),
    tx(1001, 21, D("2023-12-28"), at(2024, 1, 2, 15), "P", 5, officer=True, director=False),  # filed in 2024: unseen
    # the 2024 window
    tx(1001, 11, D("2024-09-10"), at(2024, 9, 11, 20), "S", 1000),
    tx(1001, 12, D("2024-10-21"), at(2024, 10, 22, 22, 30), "P", 500.5),     # 22:30 UTC: usable from 10-24, not 10-23
    tx(1001, 13, D("2024-10-28"), at(2024, 10, 29, 21), "P", 300),
    tx(1001, 14, D("2024-11-01"), at(2024, 11, 4, 21, 59), "P", 200),        # 21:59 UTC: usable on 11-05
    tx(1001, 21, D("2024-11-06"), at(2024, 11, 7, 20), "S", 40.25, officer=True, director=False),
    tx(1001, 20, D("2024-08-01"), at(2024, 10, 15, 21), "S", 250),           # late filing inside the window
    tx(1001, 19, D("2024-03-01"), at(2024, 10, 10, 21), "P", 400),           # late filing outside the window
    tx(1001, 15, D("2024-10-08"), at(2024, 10, 9, 20), "P", 5000, director=False),                # 10% owner only
    tx(1001, 12, D("2024-10-21"), at(2024, 10, 25, 20), "P", 999, form="4/A"),                   # amendment
    tx(1001, 16, D("2024-10-14"), at(2024, 10, 15, 20), "P", 77, form="5"),                      # Form 5
    tx(1001, 17, D("2024-10-14"), at(2024, 10, 15, 20), "P", 88, table="derivative"),            # derivative
    tx(1001, 18, D("2024-10-14"), at(2024, 10, 15, 20), "P", 99, direction="D"),                 # direction mismatch
    tx(1001, 22, D("2024-10-14"), at(2024, 10, 15, 20), "P", 0.0),                               # zero shares
    tx(1001, 23, D("2024-10-16"), at(2024, 10, 17, 20), "S", 7000, owners=3, ten=True),  # sponsor group + director
    tx(1001, 24, D("2024-09-16"), at(2024, 9, 17, 20), "P", 30, ten=True),               # founder director, 10%, alone
    tx(1001, 11, D("2024-11-20"), at(2024, 11, 21, 20), "M", 123),           # exercise: presence only
    # CIK 2002: an award only (Section 16 present, no open-market trade)
    tx(2002, 31, D("2024-06-03"), at(2024, 6, 4, 20), "A", 1000),
    tx(2002, 32, D("2024-12-02"), at(2024, 12, 3, 20), "S", 5e6),   # |net| > shares_out: ratio NaN, seller counted
    # CIK 4004: last Section 16 row 2023-10-20 -> presence ends inside the role
    tx(4004, 41, D("2023-10-19"), at(2023, 10, 20, 20), "M", 10),
    # CIK 5005 (linked from 11-01; shares_out NaN on line 505)
    tx(5005, 51, D("2024-11-12"), at(2024, 11, 13, 20), "P", 600),
    tx(5005, 52, D("2024-05-12"), at(2024, 5, 13, 20), "S", 100),
    # unlinked and sealed
    tx(7777, 71, D("2024-10-10"), at(2024, 10, 11, 20), "P", 1),
    tx(1001, 11, D("2025-01-06"), at(2025, 1, 7, 20), "S", 1),
]
INS_COLS = ("issuer_cik", "owner_cik", "form", "is_amendment", "table_type", "transaction_code", "acquired_disposed",
            "shares", "transaction_date", "any_director", "any_officer", "any_ten_percent_owner", "n_reporting_owners",
            "available_at")
INS_TYPES = {"issuer_cik": pa.int64(), "owner_cik": pa.int64(), "form": pa.string(), "is_amendment": pa.bool_(),
             "table_type": pa.string(), "transaction_code": pa.string(), "acquired_disposed": pa.string(),
             "shares": pa.float64(), "transaction_date": pa.date32(), "any_director": pa.bool_(),
             "any_officer": pa.bool_(), "any_ten_percent_owner": pa.bool_(), "n_reporting_owners": pa.int64(),
             "available_at": pa.timestamp("us")}


def insider_tables(rows=TXS, first=(2015, 1), last=(2025, 2)):
    """transactions/year=Y/YqN.parquet for every filing quarter first..last (empty ones included) + one owners file."""
    tables, (y, q) = {}, first
    while (y, q) <= last:
        sel = [r for r in rows if (r["available_at"].year, (r["available_at"].month - 1) // 3 + 1) == (y, q)]
        data = {c: pa.array([naive(r[c]) if c == "available_at" else r[c] for r in sel], INS_TYPES[c]) for c in INS_COLS}
        data["issuer_name"] = pa.array(["x"] * len(sel), pa.string())
        tables[f"transactions/year={y}/{y}q{q}.parquet"] = pa.table(data)
        y, q = (y + 1, 1) if q == 4 else (y, q + 1)
    tables["owners/year=2024/2024q4.parquet"] = pa.table({"accession": pa.array(["a"], pa.string())})
    return tables


# ---- 8-K items -----------------------------------------------------------------------------------------------------
K8 = [  # (cik, accession, form, items, available_at)
    (1001, "0000001001-23-000090", "8-K", ("7.01",), at(2023, 12, 1, 15)),
    (1001, "0000001001-24-000040", "8-K", ("1.01", "9.01"), at(2024, 6, 3, 14)),       # material, before the role
    (1001, "0000001001-24-000045", "8-K12B", ("8.01",), at(2024, 9, 3, 14)),
    (1001, "0000001001-24-000050", "8-K", ("2.02", "9.01"), at(2024, 10, 29, 22, 30)),  # usable from 10-31
    (1001, "0000001001-24-000055", "8-K", ("5.02",), at(2024, 11, 12, 15)),             # material
    (1001, "0000001001-24-000056", "8-K/A", ("5.02",), at(2024, 11, 20, 15)),           # amendment: ignored
    (1001, "0000001001-24-000060", "8-K", ("8.01",), at(2024, 12, 2, 13)),              # co-registrant with 2002
    (2002, "0000001001-24-000060", "8-K", ("8.01",), at(2024, 12, 2, 13)),
    (4004, "0000004004-23-000070", "8-K", ("8.01",), at(2023, 11, 1, 14)),              # presence ends inside the role
    (5005, "0000005005-24-000070", "8-K", ("2.05", "9.01"), at(2024, 11, 15, 21, 59)),  # 21:59 UTC: usable 11-18
    (7777, "0000007777-24-000070", "8-K", ("1.01",), at(2024, 10, 15, 14)),             # unlinked
    (1001, "0000001001-25-000070", "8-K", ("5.02",), at(2025, 1, 6, 14)),               # sealed
    (1001, "0000001001-24-000080", "10-Q", ("",), at(2024, 11, 1, 14)),                 # not an 8-K form
]


def eightk_table(rows=K8):
    flat = [(c, a, f, it, av) for c, a, f, items, av in rows for it in items]
    c = list(zip(*flat))
    return pa.table({"cik": pa.array(c[0], pa.int64()), "accession": pa.array(c[1], pa.string()),
                     "form": pa.array(c[2], pa.string()),
                     "is_amendment": pa.array([f.endswith("/A") for f in c[2]], pa.bool_()),
                     "item": pa.array(c[3], pa.string()), "available_at": pa.array([naive(x) for x in c[4]],
                                                                                     pa.timestamp("us")),
                     "filing_date": pa.array([x.date() for x in c[4]], pa.date32())})


class SecFixture(base.Fixture):
    def __init__(self, root: Path):
        super().__init__(root)
        self.stages = root / "alpha_panel"
        self.pins = {
            "earnings_calendar_sha256": write_stage(self.stages / "earnings_calendar", sec.STAGES["earnings_calendar"][1],
                                                    {"announcements.parquet": announcements_table()}),
            "insider_sha256": write_stage(self.stages / "insider", sec.STAGES["insider"][1], insider_tables()),
            "sec_filings_sha256": write_stage(self.stages / "sec_filings", sec.STAGES["sec_filings"][1],
                                              {"eight_k_items.parquet": eightk_table()})}
        self.sec_bridge = root / "sec_bridge"
        self.sec_bridge_sha = base.write_bridge(self.sec_bridge, rows=SEC_BRIDGE)

    def options(self, **over):
        o = {"sec_stages": self.stages, "sec_identity_bridge": self.sec_bridge,
             "sec_identity_bridge_sha256": self.sec_bridge_sha, **self.pins}
        o.update(over)
        return o

    def run_sec(self, out, fields, **kw):
        kw.setdefault("module_options", self.options())
        return self.run(out, fields=fields, **kw)


SEC_RUN = ["si_shares", "shares_out"] + list(sec.FIELDS)
NAN = float("nan")


# ---- the oracle ----------------------------------------------------------------------------------------------------
def oracle_ea(t, sid):
    d = SESSIONS[t]
    cik, primary = sec_link(sid, d)
    out = dict.fromkeys(sec.EA_NAMES, NAN)
    if not primary:
        return out
    vis = [r for r in ANN if r[0] == cik and r[6] and usable(r[2], d)]
    if not vis:
        return out
    a = max(vis, key=lambda r: (r[2], r[1]))
    if (d - a[4]).days > 200:
        return out
    since = IDX[d] - IDX[a[5]]
    pending = [next_expected(r[4]) for r in vis if next_expected(r[4]) > a[4] + dt.timedelta(days=45)]
    e = min(pending) if pending else None
    if e is None or e > a[4] + dt.timedelta(days=136):
        e = first_session_on_or_after(a[4] + dt.timedelta(days=91))
    to = at_or_after(e) - IDX[d]
    out.update(ea_days_since=float(since), ea_window_post3=float(0 <= since <= 3), ea_time_of_day=TIMING[a[3]],
               ea_delay_days=float(a[8]) if a[7] == "yoy_364" else NAN)
    if to >= -63:
        out.update(ea_days_to_expected=float(to), ea_window_pre5=float(1 <= to <= 5))
    return out


def trades():
    return [r for r in TXS if r["table_type"] == "non_derivative" and r["transaction_code"] in ("P", "S")
            and r["form"] == "4" and (r["any_director"] or r["any_officer"]) and r["shares"] > 0
            and not (r["n_reporting_owners"] > 1 and r["any_ten_percent_owner"])
            and r["acquired_disposed"] == ("A" if r["transaction_code"] == "P" else "D")]


def classify(cik, owner, year):
    hist = [r for r in trades() if r["issuer_cik"] == cik and r["owner_cik"] == owner
            and year - 3 <= r["transaction_date"].year <= year - 1 and r["available_at"] < at(year, 1, 1)]
    months = [{r["transaction_date"].month for r in hist if r["transaction_date"].year == y}
              for y in (year - 1, year - 2, year - 3)]
    if not all(months):
        return "unclassified"
    return "routine" if months[0] & months[1] & months[2] else "opportunistic"


def oracle_ins(t, sid, so):
    d = SESSIONS[t]
    cik, primary = sec_link(sid, d)
    out = dict.fromkeys(sec.INS_NAMES, NAN)
    if not primary:
        return out
    cutoff = mark(prev(d)) - dt.timedelta(days=365)
    if not any(r["issuer_cik"] == cik and usable(r["available_at"], d) and r["available_at"] >= cutoff for r in TXS):
        return out
    start, cstart = CAL[IDX[d] - 126], CAL[IDX[d] - 21]
    win = [r for r in trades() if r["issuer_cik"] == cik and usable(r["available_at"], d)
           and r["transaction_date"] >= start]
    sign = lambda r: r["shares"] if r["transaction_code"] == "P" else -r["shares"]
    net = sum(sign(r) for r in win)
    opp = sum(sign(r) for r in win
              if classify(cik, r["owner_cik"], r["transaction_date"].year) == "opportunistic")
    buyers = {r["owner_cik"] for r in win if r["transaction_code"] == "P"}
    sellers = {r["owner_cik"] for r in win if r["transaction_code"] == "S"}
    cluster = {r["owner_cik"] for r in trades() if r["issuer_cik"] == cik and r["transaction_code"] == "P"
               and usable(r["available_at"], d) and r["transaction_date"] >= cstart}
    out.update(ins_n_buyers=float(len(buyers)), ins_n_sellers=float(len(sellers)),
               ins_cluster_buy=float(len(cluster) >= 3))
    if math.isfinite(so) and so > 0:
        out.update({k: v / so if abs(v) <= so else NAN for k, v in (("ins_net_buy_ratio", net),
                                                                     ("ins_opportunistic_net", opp))})
    return out


def oracle_k8(t, sid):
    d = SESSIONS[t]
    cik, primary = sec_link(sid, d)
    out = dict.fromkeys(sec.K8_NAMES, NAN)
    if not primary:
        return out
    accs = {}
    for c, a, f, items, av in K8:
        if c == cik and f.startswith("8-K") and not f.endswith("/A"):
            accs[a] = (av, items)
    vis = {a: v for a, v in accs.items() if usable(v[0], d)}
    if not vis:
        return out
    latest = max(v[0] for v in vis.values())
    if latest < mark(prev(d)) - dt.timedelta(days=365):
        return out
    new = lambda w: [v for v in vis.values() if not usable(v[0], CAL[IDX[d] - w])]
    material = [v for v in new(21) if set(v[1]) & set(sec.K8_MATERIAL_ITEMS)]
    out.update(k8_count_63=float(len(new(63))), k8_item_material_21=float(bool(material)),
               k8_days_since_any=float(IDX[d] - event_session(latest)))
    return out


def oracle(names, shares_out):
    out = {x: np.full((len(SESSIONS), len(IDS)), np.nan) for x in names}
    for t in range(len(SESSIONS)):
        for i, sid in enumerate(IDS):
            cell = {**oracle_ea(t, sid), **oracle_ins(t, sid, shares_out[t, i]), **oracle_k8(t, sid)}
            for x in names:
                out[x][t, i] = cell[x]
    return out


class SecFields(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.fx = SecFixture(Path(cls.temp.name))
        cls.manifest = cls.fx.run_sec("sec", SEC_RUN)
        cls.so = cls.fx.field("sec", "shares_out")
        cls.expected = oracle(list(sec.FIELDS), cls.so)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def got(self, name):
        return self.fx.field("sec", name)

    def at_(self, name, day, sid):
        return self.got(name)[SESSIONS.index(D(day)), IDS.index(sid)]

    def test_every_field_equals_the_oracle(self):
        for name in sec.FIELDS:
            np.testing.assert_array_equal(self.got(name), self.expected[name], err_msg=name)
            self.assertTrue(np.isfinite(self.got(name)).any(), name)   # the fixture exercises every field

    def test_insider_sealed_quarter_present_on_disk_is_never_opened(self):
        # W0-1: a filing quarter that begins on or after the research seal holds only sealed rows. The fixture writes
        # and lists the seal year's q1 inside the 31-day read margin after the role's last session: it is not opened.
        sealed = f"transactions/year={rw.SEAL.year}/{rw.SEAL.year}q1.parquet"
        self.assertTrue(rw.partition_is_sealed(rw.SEAL.year, 1))
        self.assertTrue((self.fx.stages / "insider" / sealed).is_file())
        checks = self.manifest["source_checks"]["sec"]["insider"]
        self.assertEqual(checks["files_not_read_sealed"], 1)
        listed = sum(1 for k in json.loads((self.fx.stages / "insider" / "manifest.json").read_text())["files"]
                     if k.startswith("transactions/"))
        self.assertEqual(checks["files_read"], listed - checks["files_not_read_after_role"] - 1)

    def test_pit_acceptance_after_22_utc_is_not_usable_next_session(self):
        # 1001's release accepted 2024-10-29 22:30 UTC: still the July release on 10-30, the new one from 10-31
        self.assertEqual(self.at_("ea_delay_days", "2024-10-30", 101), 3.0)
        self.assertEqual(self.at_("ea_delay_days", "2024-10-31", 101), 7.0)
        self.assertEqual(self.at_("ea_days_since", "2024-10-31", 101), 1.0)   # reaction session 10-30
        # 8-K of the same filing: 10-30 still counts the 09-03 8-K12B as latest; 10-31 counts the new one
        self.assertEqual(self.at_("k8_days_since_any", "2024-10-31", 101), 1.0)
        # Form 4 accepted 22:30 UTC on 10-22: not a buyer on 10-23, a buyer on 10-24
        self.assertEqual(self.at_("ins_n_buyers", "2024-10-23", 101), 1.0)   # owner 24 only
        self.assertEqual(self.at_("ins_n_buyers", "2024-10-24", 101), 2.0)   # + owner 12
        # 21:59 UTC on d-1 is usable on d
        self.assertEqual(self.at_("ea_days_since", "2024-11-08", 202), 0.0)   # post-market 11-07 -> reaction 11-08
        self.assertEqual(self.at_("ins_cluster_buy", "2024-11-05", 101), 1.0)  # 12, 13 and 14 (usable 11-05)
        self.assertEqual(self.at_("ins_cluster_buy", "2024-11-04", 101), 0.0)

    def test_expectation_consumption_fallback_overdue_and_staleness(self):
        # 1001 before its late release: pending 2024-10-22 (yoy of 2023-10-24); overdue afterwards
        self.assertEqual(self.at_("ea_days_to_expected", "2024-10-15", 101), 5.0)
        self.assertEqual(self.at_("ea_window_pre5", "2024-10-15", 101), 1.0)
        self.assertEqual(self.at_("ea_days_to_expected", "2024-10-25", 101), -3.0)
        # after it: 2024-10-22 consumed; next is 2025-01-23 counted on the NYSE rule calendar (01-01, 01-09, 01-20 shut)
        self.assertEqual(self.at_("ea_days_to_expected", "2024-12-31", 101), 14.0)
        # 2002 has no year-ago rows: session + 91 days (2024-11-05), then 2025-02-06 after the 11-07 release
        self.assertEqual(self.at_("ea_days_to_expected", "2024-11-04", 202), 1.0)
        self.assertTrue(math.isnan(self.at_("ea_delay_days", "2024-11-08", 202)))  # prev_primary_plus_91 rule
        # 5005's 2024-10-24 release consumes the 2024-11-05 year-ago expectation: next is 2025-02-07
        self.assertEqual(self.at_("ea_days_to_expected", "2024-11-01", 505), 66.0)
        # 4004: the 2024-07-18 expectation is overdue by > 63 sessions from mid-October, stale after 2024-11-18
        self.assertEqual(self.at_("ea_days_to_expected", "2024-10-01", 404), -52.0)
        self.assertTrue(math.isnan(self.at_("ea_days_to_expected", "2024-10-17", 404)))
        self.assertEqual(self.at_("ea_days_since", "2024-11-18", 404), 137.0)   # reaction 2024-05-03
        self.assertTrue(math.isnan(self.at_("ea_days_since", "2024-11-19", 404)))
        # the J line of 1001 and the unlinked line stay NaN
        self.assertTrue(np.isnan(self.got("ea_days_since")[:, IDS.index(303)]).all())

    def test_insider_classification_windows_and_exclusions(self):
        self.assertEqual(classify(1001, 11, 2024), "routine")
        self.assertEqual(classify(1001, 12, 2024), "opportunistic")
        self.assertEqual(classify(1001, 13, 2024), "unclassified")
        self.assertEqual(classify(1001, 21, 2024), "unclassified")   # its 2023 trade was filed in 2024
        t, i = SESSIONS.index(D("2024-11-12")), IDS.index(101)
        so = self.so[t, i]
        # window: 11 S 1000, 12 P 500.5, 13 P 300, 14 P 200, 21 S 40.25, 20 S 250 (late filing, trade inside), 24 P 30
        self.assertEqual(self.got("ins_net_buy_ratio")[t, i], (500.5 + 300 + 200 + 30 - 1000 - 40.25 - 250) / so)
        self.assertEqual(self.got("ins_opportunistic_net")[t, i], 500.5 / so)
        self.assertEqual(self.got("ins_n_buyers")[t, i], 4.0)
        self.assertEqual(self.got("ins_n_sellers")[t, i], 3.0)
        # 2002: awards only -> present with zero trades; 4004: presence expires; 505: shares_out NaN
        self.assertEqual(self.at_("ins_net_buy_ratio", "2024-11-12", 202), 0.0)
        self.assertTrue(math.isnan(self.at_("ins_net_buy_ratio", "2024-12-04", 202)))   # 5e6 sold > shares_out
        self.assertEqual(self.at_("ins_n_sellers", "2024-12-04", 202), 1.0)
        self.assertEqual(self.at_("ins_n_buyers", "2024-10-21", 404), 0.0)
        self.assertTrue(math.isnan(self.at_("ins_n_buyers", "2024-10-23", 404)))
        self.assertEqual(self.at_("ins_n_buyers", "2024-11-15", 505), 1.0)
        self.assertTrue(math.isnan(self.at_("ins_net_buy_ratio", "2024-11-15", 505)))
        self.assertTrue(math.isnan(self.at_("shares_out", "2024-11-15", 505)))

    def test_eightk_windows_presence_and_links(self):
        self.assertEqual(self.at_("k8_item_material_21", "2024-11-13", 101), 1.0)   # 5.02 accepted 11-12 15:00
        self.assertEqual(self.at_("k8_item_material_21", "2024-11-12", 101), 0.0)
        self.assertEqual(self.at_("k8_count_63", "2024-12-03", 202), 1.0)           # co-registrant 8-K
        self.assertTrue(math.isnan(self.at_("k8_count_63", "2024-12-02", 202)))     # 2002 had no 8-K before
        self.assertEqual(self.at_("k8_count_63", "2024-10-31", 404), 0.0)
        self.assertTrue(math.isnan(self.at_("k8_count_63", "2024-11-01", 404)))     # presence expired
        self.assertTrue(math.isnan(self.at_("k8_count_63", "2024-10-31", 505)))     # not linked yet
        self.assertEqual(self.at_("k8_item_material_21", "2024-11-18", 505), 1.0)   # 2.05 at 21:59 UTC on 11-15

    def test_manifest_records_formula_pins_coverage_and_sources(self):
        entries = {e["name"]: e for e in self.manifest["fields"]}
        self.assertEqual([e["name"] for e in self.manifest["fields"]], SEC_RUN)
        for name, spec in sec.FIELDS.items():
            e = entries[name]
            self.assertEqual(e["formula_id"], spec["formula_id"])
            self.assertEqual(e["formula_sha256"], tool.formula_id(name, tool.spec_definition(name, 1)))
            self.assertEqual(e["lag_sessions"], 1)
            self.assertTrue(e["point_in_time"])
            self.assertEqual(e["clock"], sec.SEC_CLOCK)
            for stage in spec["stages"]:
                self.assertEqual(e["stage_manifests"][stage]["sha256"], self.fx.pins[f"{stage}_sha256"])
            self.assertEqual(e["identity_bridge_manifest_sha256"], self.fx.sec_bridge_sha)
            self.assertIn(self.fx.sec_bridge_sha, [s.get("sha256") for s in e["sources"]])
            self.assertIn("2024", e["coverage"]["per_year"])
            self.assertIn("2024", e["coverage_linked_primary"])
            self.assertEqual(set(e["nan_reasons_member_cells"]), {"not_primary_link", "absent_or_stale", "out_of_rule"})
            self.assertEqual(e["sha256"], self.manifest["files"][f"{name}.f64"]["sha256"])
        self.assertEqual(entries["ins_net_buy_ratio"]["depends_on"], ["shares_out"])
        checks = self.manifest["source_checks"]["sec"]
        self.assertEqual(checks["calendar"]["role_sessions_not_in_rule"], 2)   # the fixture keeps 11-28 and 12-25
        self.assertEqual(checks["earnings_calendar"]["lookback_truncated_rows"], 0)
        self.assertEqual(checks["insider"]["files_not_read_after_role"], 1)    # 2025q2 (2025q1: within 31 days)
        # 2025q1 begins at the seal this fixture binds (conftest.py): it is never opened, so its sealed row is not read
        self.assertEqual(checks["insider"]["files_not_read_sealed"], 1)
        self.assertEqual(checks["insider"]["rows_sealed"], 0)
        self.assertEqual(checks["insider"]["dropped_form_not_original_4"], 2)
        self.assertEqual(checks["insider"]["dropped_not_director_or_officer"], 1)
        self.assertEqual(checks["insider"]["dropped_code_direction_mismatch"], 1)
        self.assertEqual(checks["insider"]["dropped_shares_not_positive"], 1)
        self.assertEqual(checks["insider"]["dropped_joint_filing_with_ten_percent_owner"], 1)
        self.assertEqual(entries["ins_net_buy_ratio"]["domain"], [-1.0, 1.0])
        self.assertGreater(entries["ins_net_buy_ratio"]["outside_domain_member_cells"], 0)
        self.assertEqual(checks["sec_filings"]["accessions_used"], 9)

    def test_min_history_rules(self):
        with tempfile.TemporaryDirectory() as temp:
            fx = SecFixture(Path(temp))
            # insider stage from 2022q1: classification needs 2025 -> opportunistic NaN, the rest unchanged
            short = fx.base / "short" / "insider"
            fx.pins["insider_sha256"] = write_stage(short, sec.STAGES["insider"][1], insider_tables(first=(2022, 1)))
            opts = fx.options(sec_stages=fx.base / "short")
            fields = ["si_shares", "shares_out", "ins_net_buy_ratio", "ins_opportunistic_net"]
            fx.run("m22", fields=fields, module_options=opts)
            self.assertTrue(np.isnan(fx.field("m22", "ins_opportunistic_net")).all())
            np.testing.assert_array_equal(fx.field("m22", "ins_net_buy_ratio"), self.got("ins_net_buy_ratio"))
            # insider stage from 2024q3: the 126-session window starts before the data for the whole role
            late = fx.base / "late" / "insider"
            fx.pins["insider_sha256"] = write_stage(late, sec.STAGES["insider"][1], insider_tables(first=(2024, 3)))
            fx.run("m24", fields=fields, module_options=fx.options(sec_stages=fx.base / "late"))
            self.assertTrue(np.isnan(fx.field("m24", "ins_net_buy_ratio")).all())

    def test_refusals_before_any_output(self):
        opts = self.fx.options()
        for bad, msg in ((dict(sec_stages=None), "--sec-stages"), (dict(sec_identity_bridge_sha256=None), "bridge"),
                         (dict(insider_sha256=None), "--insider-sha256")):
            with self.assertRaisesRegex(ValueError, msg):
                self.fx.run("refused", fields=SEC_RUN, module_options={**opts, **bad})
            self.assertFalse((self.fx.base / "refused").exists())
        with self.assertRaisesRegex(ValueError, "requires shares_out"):
            self.fx.run("refused", fields=["ins_net_buy_ratio"], module_options=opts)
        with self.assertRaisesRegex(ValueError, "earnings-calendar manifest SHA-256"):
            self.fx.run("wrongpin", fields=["ea_days_since"], module_options={**opts, "earnings_calendar_sha256": "0" * 64})
        # a stage file that no longer matches its manifest
        with tempfile.TemporaryDirectory() as temp:
            fx = SecFixture(Path(temp))
            path = fx.stages / "sec_filings" / "eight_k_items.parquet"
            path.write_bytes(path.read_bytes() + b"\0")
            with self.assertRaisesRegex(ValueError, "does not match its manifest entry"):
                fx.run("tampered", fields=["k8_count_63"], module_options=fx.options())

    def test_command_line(self):
        out = self.fx.base / "cli"
        o = self.fx.options()
        argv = ["--role", str(self.fx.role), "--role-sha256", self.fx.role_sha, "--output", str(out),
                "--fields", "ea_days_since,k8_count_63", "--finra", str(self.fx.finra), "--tickerhistory", str(self.fx.th),
                "--lake", str(self.fx.lake), "--sec-stages", str(o["sec_stages"]),
                "--sec-identity-bridge", str(o["sec_identity_bridge"]),
                "--sec-identity-bridge-sha256", o["sec_identity_bridge_sha256"],
                "--earnings-calendar-sha256", o["earnings_calendar_sha256"],
                "--sec-filings-sha256", o["sec_filings_sha256"]]
        with contextlib.redirect_stdout(io.StringIO()):
            tool.main(argv)
        m = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(m["files"]["k8_count_63.f64"], self.manifest["files"]["k8_count_63.f64"])
        self.assertEqual(m["files"]["ea_days_since.f64"], self.manifest["files"]["ea_days_since.f64"])


class NyseCalendar(unittest.TestCase):
    def test_rule_matches_the_hand_list(self):
        got = sec.nyse_sessions(D("2023-01-01"), D("2026-12-31"))
        want = [(d - dt.date(1970, 1, 1)).days for d in _ALL if d.weekday() < 5
                and d not in HOLIDAYS]
        self.assertEqual(got.tolist(), want)

    def test_observance_and_special_closures(self):
        h = lambda y: sec.nyse_holidays(y)
        self.assertIn(D("2021-12-24"), h(2021))          # Christmas on Saturday -> Friday
        self.assertNotIn(D("2021-12-31"), h(2021))       # New Year 2022 on Saturday: not observed
        self.assertNotIn(D("2021-12-31"), h(2022))
        self.assertIn(D("2022-06-20"), h(2022))          # Juneteenth on Sunday -> Monday
        self.assertNotIn(D("2021-06-18"), h(2021))       # no Juneteenth before 2022
        self.assertIn(D("2021-04-02"), h(2021))          # Good Friday
        self.assertIn(D("2018-12-05"), h(2018))          # special closure
        self.assertIn(D("2022-12-26"), h(2022))


class ByteIdentity(unittest.TestCase):
    """Every other field's bytes, manifest entry and source check are identical with and without the SEC fields."""

    def test_full_recipe_with_and_without_sec(self):
        with tempfile.TemporaryDirectory() as temp:
            fx = svt.SvFixture(Path(temp))
            stages = SecFixture.__new__(SecFixture)
            stages.base = fx.base
            stages.stages = fx.base / "alpha_panel"
            stages.pins = {
                "earnings_calendar_sha256": write_stage(stages.stages / "earnings_calendar",
                                                        sec.STAGES["earnings_calendar"][1],
                                                        {"announcements.parquet": announcements_table()}),
                "insider_sha256": write_stage(stages.stages / "insider", sec.STAGES["insider"][1], insider_tables()),
                "sec_filings_sha256": write_stage(stages.stages / "sec_filings", sec.STAGES["sec_filings"][1],
                                                  {"eight_k_items.parquet": eightk_table()})}
            stages.sec_bridge = fx.base / "sec_bridge"
            stages.sec_bridge_sha = base.write_bridge(stages.sec_bridge, rows=SEC_BRIDGE)
            bridge, events = fx.base / "bridge", fx.base / "events"
            kw = dict(identity_bridge=bridge, identity_bridge_sha256=base.write_bridge(bridge),
                      fund_events=events, fund_events_sha256=base.write_events(events))
            recipe = list(tool.FIELDS) + list(tool.ISSUER_FIELDS) + ["sv_ratio126"]
            without = fx.run("without", fields=recipe, **kw)
            with_sec = fx.run("with", fields=recipe + list(sec.FIELDS), module_options=SecFixture.options(stages), **kw)
            self.assertEqual([e["name"] for e in with_sec["fields"]], recipe + list(sec.FIELDS))
            for name in recipe:
                a, b = (fx.base / "without" / f"{name}.f64").read_bytes(), (fx.base / "with" / f"{name}.f64").read_bytes()
                self.assertEqual(a, b, name)
                self.assertEqual(without["files"][f"{name}.f64"], with_sec["files"][f"{name}.f64"], name)
                ea = next(e for e in without["fields"] if e["name"] == name)
                eb = next(e for e in with_sec["fields"] if e["name"] == name)
                self.assertEqual(ea, eb, name)
            for key, value in without["source_checks"].items():
                self.assertEqual(with_sec["source_checks"][key], value, key)
            self.assertEqual(set(with_sec["source_checks"]) - set(without["source_checks"]), {"sec"})
            # the SEC fields do not depend on which other fields ran
            alone = fx.run("alone", fields=SEC_RUN, module_options=SecFixture.options(stages))
            for name in sec.FIELDS:
                self.assertEqual(alone["files"][f"{name}.f64"], with_sec["files"][f"{name}.f64"], name)
            # --reuse never copies a SEC field (its group has no reuse roots): recomputed, identical bytes
            again = fx.run("again", fields=SEC_RUN, module_options=SecFixture.options(stages), reuse=fx.base / "alone")
            self.assertEqual(set(again["reuse"]["computed"]), set(sec.FIELDS))
            for name in sec.FIELDS:
                self.assertEqual(again["files"][f"{name}.f64"], alone["files"][f"{name}.f64"], name)
        self.assertEqual(tool.DEFAULT_FIELDS, ("si_shares", "si_dtc", "iv_atm_21d", "iv_atm_63d", "iv_atm_126d",
                                               "earn_recent", "shares_out", "mkt_ret"))
        self.assertEqual(list(tool.ALL_FIELDS)[:len(recipe)], recipe)
        self.assertLessEqual(len(recipe) + len(sec.FIELDS), 64)   # runner field limit


if __name__ == "__main__":
    unittest.main()
