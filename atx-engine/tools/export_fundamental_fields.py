"""Export point-in-time fundamental snapshots for the identified equity panels (lane 10).

Reads SEC Company Facts (the bulk ``companyfacts.zip`` the atx-db warehouse
ingests) and the warehouse's security-identifier bridge, and writes one row per
(panel security, filing) describing what an investor could know about the
company the moment that filing became public. The C++ side
(``atx/engine/data/fundamental_fields.hpp``) owns the as-of join onto a panel
axis, the lag, the staleness caps and every price-scaled ratio; this tool owns
only the fiscal-period arithmetic that needs a company's statement history:
trailing-twelve-month sums, year-ago balances and standardized unexpected
earnings.

Point-in-time discipline
------------------------
* A fact is known at filing F only if its own ``filed`` date is <= F's filed
  date. Restatements therefore enter on their own filing date, never earlier.
* ``available_date`` = filed + 1 calendar day: the warehouse clock policy
  ``sec_filed_date_plus_46h_v1`` makes a Company Facts fact eligible at 22:00
  UTC of the day after filing, which the daily 22:00 UTC decision cutoff of the
  session labelled filed+1 can use. Consumers add their own session lag on top.
* Nothing available on or after ``--seal`` (default 2020-01-01, the validation
  seal of the equity program) is ever written.

Inputs are opened read-only; the output directory must not exist.

Usage::

    python atx-engine/tools/export_fundamental_fields.py \
        --companyfacts C:/atx/atx-db/data/cache/companyfacts.zip \
        --warehouse C:/atx/atx-db/data/warehouse.duckdb.pre-migrate.20260922-235808.bak \
        --contexts "C:/atx/data/equity_scorecard16_ctx_*_t*_20260920" \
        --out C:/atx/data/equity_fund_fields_l10_20260923
"""
from __future__ import annotations

import argparse
import bisect
import csv
import datetime as dt
import glob
import hashlib
import json
import math
import os
import statistics
import struct
import sys
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

TOOL_VERSION = "fundamental-fields-export-v1"
CLOCK_POLICY = "sec_filed_date_plus_46h_v1 (available_date = filed + 1 day)"

# Raw output columns, in the exact order of fundamental_fields.hpp RawField.
RAW_FIELDS = (
    "book_equity",
    "total_assets",
    "total_liabilities",
    "assets_lag1y",
    "net_income_ttm",
    "revenue_ttm",
    "gross_profit_ttm",
    "operating_cash_flow_ttm",
    "operating_income_ttm",
    "shares_outstanding",
    "shares_lag1y",
    "sue",
)
KEY_COLUMNS = ("sr_id", "cik", "filed", "available_date", "period_end", "form", "accn")

# Concept priority lists (us-gaap unless noted). Earlier = preferred on a tie.
C_EQUITY = ("StockholdersEquity",)
C_EQUITY_NCI = ("StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest",)
C_ASSETS = ("Assets",)
C_LIABILITIES = ("Liabilities",)
C_NET_INCOME = ("NetIncomeLoss", "ProfitLoss", "NetIncomeLossAvailableToCommonStockholdersBasic")
C_REVENUE = (
    "Revenues",
    "RevenueFromContractWithCustomerExcludingAssessedTax",
    "SalesRevenueNet",
    "RevenueFromContractWithCustomerIncludingAssessedTax",
    "SalesRevenueGoodsNet",
    "RevenuesNetOfInterestExpense",
)
C_GROSS_PROFIT = ("GrossProfit",)
C_COST_OF_REVENUE = (
    "CostOfRevenue",
    "CostOfGoodsAndServicesSold",
    "CostOfGoodsSold",
)
C_CFO = (
    "NetCashProvidedByUsedInOperatingActivities",
    "NetCashProvidedByUsedInOperatingActivitiesContinuingOperations",
)
C_OPERATING_INCOME = ("OperatingIncomeLoss",)
C_EPS = ("EarningsPerShareDiluted", "EarningsPerShareBasicAndDiluted", "EarningsPerShareBasic")
C_SHARES_DEI = ("EntityCommonStockSharesOutstanding",)
C_SHARES_GAAP = ("CommonStockSharesOutstanding",)

EVENT_FORMS = {"10-K", "10-Q", "10-K/A", "10-Q/A", "10-KT", "10-QT", "20-F", "20-F/A", "40-F", "40-F/A"}

DAY = dt.timedelta(days=1)


def parse_date(s: str | None) -> dt.date | None:
    if not s:
        return None
    try:
        return dt.date.fromisoformat(s)
    except ValueError:
        return None


# ---------------------------------------------------------------------------
# Fiscal-period arithmetic (pure; unit-tested in tests/tools)
# ---------------------------------------------------------------------------

def duration_days(start: dt.date, end: dt.date) -> int:
    return (end - start).days + 1


def is_quarter(days: int) -> bool:
    return 80 <= days <= 100


def is_annual(days: int) -> bool:
    return 350 <= days <= 380


def derive_quarters(durations: dict[tuple[dt.date, dt.date], float]) -> dict[dt.date, tuple[dt.date, float]]:
    """Discrete 3-month values keyed by period end -> (start, value).

    Direct 3-month facts win; otherwise a quarter is the difference of two
    cumulative (year-to-date) facts that share a start date and whose ends are
    one quarter apart (6M - 3M, 9M - 6M, FY - 9M).
    """
    quarters: dict[dt.date, tuple[dt.date, float]] = {}
    for (start, end), val in durations.items():
        if is_quarter(duration_days(start, end)):
            quarters[end] = (start, val)
    by_start: dict[dt.date, list[tuple[dt.date, float]]] = {}
    for (start, end), val in durations.items():
        if duration_days(start, end) <= 380:
            by_start.setdefault(start, []).append((end, val))
    for start, rows in by_start.items():
        rows.sort()
        for (e0, v0), (e1, v1) in zip(rows, rows[1:]):
            gap = (e1 - e0).days
            if 80 <= gap <= 100 and not _has_quarter_near(quarters, e1):
                quarters[e1] = (e0 + DAY, v1 - v0)
    return quarters


def _has_quarter_near(quarters: dict[dt.date, tuple[dt.date, float]], end: dt.date, tol: int = 3) -> bool:
    return any(abs((e - end).days) <= tol for e in quarters)


def _find_near(keys: Iterable[dt.date], target: dt.date, tol: int) -> dt.date | None:
    best = None
    for k in keys:
        d = abs((k - target).days)
        if d <= tol and (best is None or d < abs((best - target).days)):
            best = k
    return best


def ttm_series(durations: dict[tuple[dt.date, dt.date], float]) -> dict[dt.date, float]:
    """Trailing-twelve-month values keyed by period end.

    A direct 12-month fact wins; otherwise four chained discrete quarters (each
    quarter's end within 10 days of the day before its successor's start).
    """
    out: dict[dt.date, float] = {}
    for (start, end), val in durations.items():
        if is_annual(duration_days(start, end)):
            out[end] = val
    quarters = derive_quarters(durations)
    for end, (start, val) in quarters.items():
        if end in out:
            continue
        total, cur_start, ok = val, start, True
        for _ in range(3):
            prev = _find_near(quarters.keys(), cur_start - DAY, 10)
            if prev is None:
                ok = False
                break
            p_start, p_val = quarters[prev]
            total += p_val
            cur_start = p_start
        if ok:
            out[end] = total
    return out


def latest(series: dict[dt.date, float]) -> tuple[dt.date, float] | None:
    if not series:
        return None
    end = max(series)
    return end, series[end]


def sue_from_quarters(eps_quarters: dict[dt.date, tuple[dt.date, float]], min_history: int = 4,
                      window: int = 8) -> tuple[dt.date, float] | None:
    """Standardized unexpected earnings of the latest quarter (seasonal random walk).

    SUE = (EPS_q - EPS_{q-4}) / sd(previous `window` seasonal differences),
    Bernard-Thomas (1989) / Livnat-Mendenhall (2006) form. Requires at least
    `min_history` previous differences and a positive standard deviation.
    """
    ends = sorted(eps_quarters)
    diffs: list[tuple[dt.date, float]] = []
    for e in ends:
        prior = _find_near(ends, e - dt.timedelta(days=365), 20)
        if prior is not None and prior != e:
            diffs.append((e, eps_quarters[e][1] - eps_quarters[prior][1]))
    if len(diffs) < min_history + 1:
        return None
    cur_end, cur = diffs[-1]
    if cur_end != ends[-1]:
        return None
    hist = [d for _, d in diffs[-(window + 1):-1]]
    if len(hist) < min_history:
        return None
    sd = statistics.stdev(hist)
    if not (sd > 0.0) or not math.isfinite(sd):
        return None
    return cur_end, cur / sd


def latest_instant(instants: dict[dt.date, float]) -> tuple[dt.date, float] | None:
    return latest(instants)


def instant_near(instants: dict[dt.date, float], target: dt.date, tol: int) -> float | None:
    k = _find_near(instants.keys(), target, tol)
    return None if k is None else instants[k]


# ---------------------------------------------------------------------------
# Company knowledge state, advanced filing by filing
# ---------------------------------------------------------------------------

@dataclass
class Fact:
    concept: str
    start: dt.date | None
    end: dt.date
    val: float
    filed: dt.date
    form: str
    accn: str


@dataclass
class Knowledge:
    """What is known about one company, as of the last applied filing date."""
    durations: dict[str, dict[tuple[dt.date, dt.date], float]] = field(default_factory=dict)
    instants: dict[str, dict[dt.date, float]] = field(default_factory=dict)
    shares: dict[dt.date, float] = field(default_factory=dict)  # dei, summed over classes

    def apply(self, facts: list[Fact]) -> None:
        dei_by_key: dict[tuple[dt.date, str], float] = {}
        for f in facts:
            if f.concept in C_SHARES_DEI:
                dei_by_key[(f.end, f.accn)] = dei_by_key.get((f.end, f.accn), 0.0) + f.val
            elif f.start is not None:
                self.durations.setdefault(f.concept, {})[(f.start, f.end)] = f.val
            else:
                self.instants.setdefault(f.concept, {})[f.end] = f.val
        for (end, _accn), total in sorted(dei_by_key.items()):
            self.shares[end] = total

    def best_ttm(self, concepts: tuple[str, ...]) -> tuple[dt.date, float] | None:
        best = None
        for c in concepts:
            cand = latest(ttm_series(self.durations.get(c, {})))
            if cand is not None and (best is None or cand[0] > best[0]):
                best = cand
        return best

    def best_instant(self, concepts: tuple[str, ...]) -> tuple[dt.date, float] | None:
        best = None
        for c in concepts:
            cand = latest_instant(self.instants.get(c, {}))
            if cand is not None and (best is None or cand[0] > best[0]):
                best = cand
        return best

    def instant_series(self, concepts: tuple[str, ...]) -> dict[dt.date, float]:
        merged: dict[dt.date, float] = {}
        for c in reversed(concepts):  # earlier concepts overwrite later ones
            merged.update(self.instants.get(c, {}))
        return merged


def snapshot(k: Knowledge) -> tuple[dt.date | None, dict[str, float]]:
    """Derive the raw fields from the current knowledge.

    Returns (period_end, values). A field whose own period end is more than 100
    days older than the snapshot's period end is left NaN so the consumer's
    per-field as-of selection keeps the older filing's value under that older
    period's staleness clock rather than relabelling it as current.
    """
    nan = float("nan")
    vals = {name: nan for name in RAW_FIELDS}
    parts: dict[str, tuple[dt.date, float]] = {}

    assets = k.best_instant(C_ASSETS)
    equity = k.best_instant(C_EQUITY) or k.best_instant(C_EQUITY_NCI)
    liabilities = k.best_instant(C_LIABILITIES)
    if assets is not None:
        parts["total_assets"] = assets
        lag = instant_near(k.instant_series(C_ASSETS), assets[0] - dt.timedelta(days=365), 20)
        if lag is not None:
            parts["assets_lag1y"] = (assets[0], lag)
        if liabilities is None or liabilities[0] < assets[0]:
            eq_nci = k.best_instant(C_EQUITY_NCI) or equity
            if eq_nci is not None and eq_nci[0] == assets[0]:
                liabilities = (assets[0], assets[1] - eq_nci[1])
    if equity is not None:
        parts["book_equity"] = equity
    if liabilities is not None:
        parts["total_liabilities"] = liabilities

    for name, concepts in (("net_income_ttm", C_NET_INCOME), ("revenue_ttm", C_REVENUE),
                           ("operating_cash_flow_ttm", C_CFO),
                           ("operating_income_ttm", C_OPERATING_INCOME)):
        got = k.best_ttm(concepts)
        if got is not None:
            parts[name] = got
    gp = k.best_ttm(C_GROSS_PROFIT)
    rev = parts.get("revenue_ttm")
    if rev is not None and (gp is None or gp[0] < rev[0]):
        cost = k.best_ttm(C_COST_OF_REVENUE)
        if cost is not None and cost[0] == rev[0]:
            gp = (rev[0], rev[1] - cost[1])
    if gp is not None:
        parts["gross_profit_ttm"] = gp

    shares_series = dict(k.shares) or k.instant_series(C_SHARES_GAAP)
    sh = latest(shares_series)
    if sh is not None:
        parts["shares_outstanding"] = sh
        lag = instant_near(shares_series, sh[0] - dt.timedelta(days=365), 45)
        if lag is not None:
            parts["shares_lag1y"] = (sh[0], lag)

    for c in C_EPS:
        q = derive_quarters(k.durations.get(c, {}))
        if q:
            got = sue_from_quarters(q)
            if got is not None:
                parts["sue"] = got
            break

    # The snapshot's fiscal period: the latest financial-statement period seen.
    # Shares (a cover-page date) never define it.
    statement_ends = [v[0] for n, v in parts.items() if n not in ("shares_outstanding", "shares_lag1y")]
    if not statement_ends:
        return None, vals
    period_end = max(statement_ends)
    for name, (end, value) in parts.items():
        is_shares = name in ("shares_outstanding", "shares_lag1y")
        if is_shares or (period_end - end).days <= 100:
            vals[name] = float(value)
    return period_end, vals


def parse_company_facts(doc: dict) -> list[Fact]:
    """Flatten a Company Facts JSON into Fact rows usable for PIT reconstruction."""
    wanted = set(C_EQUITY + C_EQUITY_NCI + C_ASSETS + C_LIABILITIES + C_NET_INCOME + C_REVENUE
                 + C_GROSS_PROFIT + C_COST_OF_REVENUE + C_CFO + C_OPERATING_INCOME + C_EPS
                 + C_SHARES_GAAP)
    out: list[Fact] = []
    facts = doc.get("facts", {})
    for taxonomy, concepts in facts.items():
        for concept, body in concepts.items():
            if taxonomy == "dei":
                if concept not in C_SHARES_DEI:
                    continue
            elif taxonomy != "us-gaap" or concept not in wanted:
                continue
            for unit, rows in body.get("units", {}).items():
                if unit not in ("USD", "shares", "USD/shares"):
                    continue
                for r in rows:
                    filed = parse_date(r.get("filed"))
                    end = parse_date(r.get("end"))
                    form = r.get("form") or ""
                    val = r.get("val")
                    if filed is None or end is None or val is None:
                        continue
                    if concept not in C_SHARES_DEI and form not in EVENT_FORMS:
                        continue
                    if end > filed:
                        continue  # a period ending after its own filing is not a fact yet
                    out.append(Fact(concept, parse_date(r.get("start")), end, float(val), filed,
                                    form, r.get("accn") or ""))
    out.sort(key=lambda f: (f.filed, f.accn))
    return out


def company_snapshots(facts: list[Fact], seal: dt.date) -> list[dict]:
    """One row per event filing date: the knowledge as of that date's close."""
    rows: list[dict] = []
    k = Knowledge()
    i = 0
    while i < len(facts):
        filed = facts[i].filed
        j = i
        while j < len(facts) and facts[j].filed == filed:
            j += 1
        group = facts[i:j]
        k.apply(group)
        i = j
        available = filed + DAY
        if available >= seal:
            break
        events = [f for f in group if f.form in EVENT_FORMS]
        if not events:
            continue
        period_end, vals = snapshot(k)
        if period_end is None:
            continue
        main = max(events, key=lambda f: (f.end, f.form.startswith("10-K")))
        rows.append({"filed": filed, "available_date": available, "period_end": period_end,
                     "form": main.form, "accn": main.accn, **vals})
    return rows


# ---------------------------------------------------------------------------
# Panel axes (identified contexts) and the warehouse id bridge
# ---------------------------------------------------------------------------

@dataclass
class Context:
    name: str
    path: Path
    instrument_ids: list[str]
    session_keys: list[int]


def load_contexts(pattern: str) -> list[Context]:
    out = []
    for d in sorted(glob.glob(pattern)):
        man_path = Path(d) / "context.bin.manifest.json"
        if not man_path.exists():
            continue
        man = json.loads(man_path.read_text())
        axes = man["axes"]
        if axes.get("instrument_namespace") != "spiderrock.securityID":
            raise SystemExit(f"{d}: unexpected instrument namespace {axes.get('instrument_namespace')}")
        out.append(Context(Path(d).name, Path(d), [str(x) for x in axes["instrument_ids"]],
                           [int(x) for x in axes["session_keys"]]))
    if not out:
        raise SystemExit(f"no identified contexts match {pattern}")
    return out


def read_apnl_mask(path: Path) -> tuple[int, int, bytes]:
    """Universe mask of an APNLv1 panel (dates x instruments, 1 = in universe)."""
    with open(path, "rb") as fh:
        head = fh.read(32)
        magic, ver, dates, insts, nfields = struct.unpack("<IIQQQ", head)
        if magic != 0x4C4E5041 or ver != 1:
            raise SystemExit(f"{path}: not an APNLv1 panel")
        pos = 32
        for _ in range(nfields):
            fh.seek(pos)
            (n,) = struct.unpack("<I", fh.read(4))
            pos += 4 + n
        pos += nfields * dates * insts * 8
        fh.seek(pos)
        mask = fh.read(dates * insts)
    return dates, insts, mask


def load_id_bridge(warehouse: str, sr_ids: set[str]) -> tuple[dict[str, str], dict[str, str]]:
    """SpiderRock security id -> 10-digit CIK, via the warehouse identifier history.

    Only ids whose warehouse security is a SEC-CIK security map; an id bridged to
    more than one CIK is ambiguous and reported, never guessed.
    """
    import duckdb  # local import: only the real export needs it

    con = duckdb.connect(warehouse, read_only=True)
    con.execute("set enable_progress_bar=false")
    rows = con.execute(
        "select id_value, security_id from security_identifier_history "
        "where id_type = 'TBLTICKERHISTORY_SECURITY_ID'").fetchall()
    con.close()
    cands: dict[str, set[str]] = {}
    for sr, sec in rows:
        if sr in sr_ids and sec.startswith("SEC-CIK-"):
            cands.setdefault(sr, set()).add(sec.removeprefix("SEC-CIK-"))
    mapping, reasons = {}, {}
    for sr in sr_ids:
        c = cands.get(sr, set())
        if len(c) == 1:
            mapping[sr] = next(iter(c))
        else:
            reasons[sr] = "ambiguous" if c else "no_sec_cik_bridge"
    return mapping, reasons


# ---------------------------------------------------------------------------
# Availability audit
# ---------------------------------------------------------------------------

def quantiles(xs: list[float], qs=(0.05, 0.25, 0.5, 0.75, 0.95)) -> dict[str, float | None]:
    if not xs:
        return {f"p{int(q * 100)}": None for q in qs}
    s = sorted(xs)
    return {f"p{int(q * 100)}": s[min(len(s) - 1, int(q * (len(s) - 1) + 0.5))] for q in qs}


def audit_context(ctx: Context, rows_by_sr: dict[str, list[dict]], lag_sessions: int,
                  cap_avail: int, cap_period: int) -> dict:
    """Coverage of in-universe cells in the context's evaluation (final) calendar year.

    Mirrors fundamental_fields.hpp::align_pit_records: visibility = first session
    at/after available_date plus `lag_sessions`; the visible record with the
    latest period (ties: later availability) wins per field; both staleness caps.
    """
    import numpy as np

    dates, insts, mask_bytes = read_apnl_mask(ctx.path / "context.bin")
    keys = np.asarray(ctx.session_keys, dtype=np.int64)
    assert dates == len(keys) and insts == len(ctx.instrument_ids)
    mask = np.frombuffer(mask_bytes, dtype=np.uint8).reshape(dates, insts)
    day_ns = 86_400 * 10**9
    epoch = dt.date(1970, 1, 1)
    last_day = epoch + dt.timedelta(days=int(keys[-1] // day_ns))
    year_start = dt.date(last_day.year, 1, 1)
    t0 = int(np.searchsorted(keys, (year_start - epoch).days * day_ns, side="left"))
    key_days = keys[t0:] // day_ns
    in_univ = mask[t0:] != 0
    fields = ("book_equity", "net_income_ttm", "gross_profit_ttm", "sue", "shares_lag1y")
    covered = {f: 0 for f in fields}
    age_days: list[np.ndarray] = []
    for i, sr in enumerate(ctx.instrument_ids):
        rows = rows_by_sr.get(sr)
        col = in_univ[:, i]
        if not rows or not col.any():
            continue
        vis = []
        for r in rows:
            a_ns = (r["available_date"] - epoch).days * day_ns
            vis.append((int(np.searchsorted(keys, a_ns, side="left")) + lag_sessions, r))
        vis.sort(key=lambda x: x[0])
        for f in fields:
            idx, best, best_rows = [], None, []
            for v, r in vis:
                if not math.isfinite(r[f]):
                    continue
                if best is None or (r["period_end"], r["available_date"]) >= (best["period_end"], best["available_date"]):
                    best = r
                idx.append(v)
                best_rows.append(best)
            if not idx:
                continue
            pos = np.searchsorted(np.asarray(idx), np.arange(t0, dates), side="right") - 1
            has = pos >= 0
            avail = np.array([(b["available_date"] - epoch).days for b in best_rows])
            pend = np.array([(b["period_end"] - epoch).days for b in best_rows])
            p = np.clip(pos, 0, None)
            ok = has & ((key_days - avail[p]) <= cap_avail) & ((key_days - pend[p]) <= cap_period) & col
            covered[f] += int(ok.sum())
            if f == "book_equity":
                age_days.append((key_days - pend[p])[ok])
    n_univ = int(in_univ.sum())
    ages = np.concatenate(age_days).astype(float).tolist() if age_days else []
    return {
        "context": ctx.name,
        "evaluation_year": year_start.year,
        "in_universe_cells": n_univ,
        "coverage": {f: (covered[f] / n_univ if n_univ else None) for f in fields},
        "book_equity_days_since_period_end": quantiles(ages),
    }


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--companyfacts", required=True)
    ap.add_argument("--warehouse", required=True, help="warehouse DuckDB (opened read-only)")
    ap.add_argument("--contexts", required=True, help="glob of identified context directories")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seal", default="2020-01-01")
    ap.add_argument("--lag-sessions", type=int, default=1)
    ap.add_argument("--max-days-since-available", type=int, default=400)
    ap.add_argument("--max-days-since-period-end", type=int, default=550)
    args = ap.parse_args(argv)

    out = Path(args.out)
    if out.exists():
        raise SystemExit(f"{out} exists; exports always go to a fresh directory")
    seal = dt.date.fromisoformat(args.seal)
    contexts = load_contexts(args.contexts)
    sr_ids = sorted({sr for c in contexts for sr in c.instrument_ids})
    mapping, unmapped = load_id_bridge(args.warehouse, set(sr_ids))

    zf = zipfile.ZipFile(args.companyfacts)
    members = set(zf.namelist())
    rows_by_sr: dict[str, list[dict]] = {}
    lag_by_form: dict[str, list[int]] = {}
    ciks_missing_facts = 0
    by_cik: dict[str, list[str]] = {}
    for sr, cik in mapping.items():
        by_cik.setdefault(cik, []).append(sr)
    for n, (cik, srs) in enumerate(sorted(by_cik.items())):
        member = f"CIK{cik}.json"
        if member not in members:
            ciks_missing_facts += 1
            for sr in srs:
                unmapped[sr] = "cik_absent_from_companyfacts"
            continue
        doc = json.loads(zf.read(member))
        snaps = company_snapshots(parse_company_facts(doc), seal)
        for s in snaps:
            lag_by_form.setdefault(s["form"], []).append((s["filed"] - s["period_end"]).days)
        for sr in srs:
            rows_by_sr[sr] = [dict(s, sr_id=sr, cik=cik) for s in snaps]
        if n % 100 == 0:
            print(f"[export] {n}/{len(by_cik)} companies", file=sys.stderr)

    out.mkdir(parents=True)
    points = out / "points.csv"
    n_rows = 0
    with open(points, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(KEY_COLUMNS + RAW_FIELDS)
        for sr in sorted(rows_by_sr, key=lambda s: (len(s), s)):
            for r in rows_by_sr[sr]:
                w.writerow([r["sr_id"], r["cik"], r["filed"].isoformat(), r["available_date"].isoformat(),
                            r["period_end"].isoformat(), r["form"], r["accn"]]
                           + [("" if not math.isfinite(r[f]) else repr(r[f])) for f in RAW_FIELDS])
                n_rows += 1

    audits = [audit_context(c, rows_by_sr, args.lag_sessions, args.max_days_since_available,
                            args.max_days_since_period_end) for c in contexts]
    unmapped_counts: dict[str, int] = {}
    for reason in unmapped.values():
        unmapped_counts[reason] = unmapped_counts.get(reason, 0) + 1
    audit = {
        "schema": "atx.fundamental-fields-availability/v1",
        "panel_securities": len(sr_ids),
        "securities_with_snapshots": sum(1 for v in rows_by_sr.values() if v),
        "unmapped_by_reason": unmapped_counts,
        "filing_lag_days_by_form": {f: {"n": len(v), **quantiles([float(x) for x in v])}
                                    for f, v in sorted(lag_by_form.items())},
        "contexts": audits,
        "alignment_used_for_coverage": {
            "lag_sessions": args.lag_sessions,
            "max_days_since_available": args.max_days_since_available,
            "max_days_since_period_end": args.max_days_since_period_end,
        },
    }
    (out / "availability_audit.json").write_text(json.dumps(audit, indent=2, default=str))
    (out / "unmapped_securities.csv").write_text(
        "sr_id,reason\n" + "".join(f"{k},{v}\n" for k, v in sorted(unmapped.items())))
    manifest = {
        "schema": "atx.fundamental-fields-export/v1",
        "tool": TOOL_VERSION,
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "clock_policy": CLOCK_POLICY,
        "seal_exclusive": seal.isoformat(),
        "inputs": {
            "companyfacts": {"path": args.companyfacts, "size": os.path.getsize(args.companyfacts),
                             "sha256": sha256_file(Path(args.companyfacts))},
            "warehouse_bridge": {"path": args.warehouse, "size": os.path.getsize(args.warehouse),
                                 "table": "security_identifier_history",
                                 "id_type": "TBLTICKERHISTORY_SECURITY_ID"},
            "contexts": [c.name for c in contexts],
        },
        "columns": {"keys": list(KEY_COLUMNS), "raw_fields": list(RAW_FIELDS)},
        "consumer": "atx/engine/data/fundamental_fields.hpp (RawField order == raw_fields)",
        "outputs": {"points.csv": {"rows": n_rows, "sha256": sha256_file(points)}},
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(json.dumps({"rows": n_rows, "mapped": len(mapping), "panel_securities": len(sr_ids)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
