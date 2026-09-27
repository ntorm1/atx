"""Export point-in-time fundamental snapshots for the identified equity panels (lane 10).

The default dated-links-v2 path consumes a sealed Company Facts projection and
a hash-bound dated security-link artifact. It writes interval records with
independent issuer-link and filing clocks; these are NOT verified first-public
fundamental timestamps. Explicit legacy-static-v1 reproduces the old warehouse
bridge and CSV. The C++ side
(``atx/engine/data/fundamental_fields.hpp``) owns the as-of join onto a panel
axis, the lag, the staleness caps and every price-scaled ratio; this tool owns
only the fiscal-period arithmetic that needs a company's statement history:
trailing-twelve-month sums, year-ago balances and standardized unexpected
earnings.

The explicit acceptance-v2 path additionally requires a hash-bound sealed
issuer/accession clock projection. Acceptance and observed public availability
are separate; unsupported dissemination and filed+46h are labeled modeled.
True DEI entity shares are a separate output, never the legacy diluted-share
field and never an inferred security-line market cap.

Legacy point-in-time discipline (numerical reproduction)
------------------------------------------------------
* A fact is known at filing F only if its own ``filed`` date is <= F's filed
  date. Restatements therefore enter on their own filing date, never earlier.
* ``available_date`` = filed + 1 calendar day: the warehouse clock policy
  ``sec_filed_date_plus_46h_v1`` label is historical: the interval writer actually
  stamps midnight UTC of filed+1 (+24h). This exact legacy clock is preserved;
  acceptance-v2 alone implements the explicit +46h modeled fallback.
* Nothing available on or after ``--seal`` (default 2020-01-01, the validation
  seal of the equity program) is ever written.

Inputs are opened read-only; the output directory must not exist.

Usage::

    python atx-engine/tools/export_fundamental_fields.py \
        --companyfacts sealed-companyfacts.zip \
        --companyfacts-sealed-manifest sealed-companyfacts.manifest.json \
        --security-links dated-links \
        --contexts "identified-contexts/*" --out fresh-export
"""
from __future__ import annotations

import argparse
import bisect
import csv
import datetime as dt
import glob
import hashlib
import importlib.util
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

TOOL_VERSION = "fundamental-fields-export-v3"
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
C_WASO = ("WeightedAverageNumberOfDilutedSharesOutstanding", "WeightedAverageNumberOfSharesOutstandingBasic")

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

# A one-year change in weighted-average shares of 100x or more is not a real
# issuance for a listed name; it is an XBRL scale error (values tagged in
# thousands or millions: ratios near 10^+-3 / 10^+-6) or a unit mix-up.
SHARE_MAX_LOG10_CHANGE = 2.0
SHARE_PAIR_RULE = "drop (shares, shares_lag1y) when |log10(shares/shares_lag1y)| >= 2 or either <= 0"
SHARE_PAIRS_REJECTED = [0]  # export-wide counter, reported in the manifest


def plausible_share_pair(current: float, lag: float) -> bool:
    """False for a (shares, shares 1y earlier) pair that carries an XBRL scale error."""
    if not (math.isfinite(current) and math.isfinite(lag)) or current <= 0.0 or lag <= 0.0:
        return False
    return abs(math.log10(current / lag)) < SHARE_MAX_LOG10_CHANGE


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
                      window: int = 8, *, finite_statistics: bool = False) -> tuple[dt.date, float] | None:
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
    if finite_statistics and not all(math.isfinite(value) for value in [cur, *hist]):
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


def snapshot(k: Knowledge, *, finite_statistics: bool = False) -> tuple[dt.date | None, dict[str, float]]:
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

    # Share counts: weighted-average diluted (else basic) shares of the latest
    # discrete period, and the same concept ~1 year earlier AS CURRENTLY KNOWN.
    # Filings restate prior-year comparatives for splits, so the knowledge set's
    # latest-filed value keeps the pair split-consistent; cover-page dei counts
    # are not restated and would book a 7:1 split as a 600% issuance.
    for c in C_WASO:
        durs = k.durations.get(c, {})
        series = {e: v for (st, e), v in durs.items() if is_quarter(duration_days(st, e))}
        if not series:
            series = {e: v for (st, e), v in durs.items() if is_annual(duration_days(st, e))}
        sh = latest(series)
        if sh is None:
            continue
        lag = instant_near(series, sh[0] - dt.timedelta(days=365), 20)
        if lag is not None and not plausible_share_pair(sh[1], lag):
            SHARE_PAIRS_REJECTED[0] += 1
            # XBRL scale error on one side (thousands vs units, ...): which side
            # is wrong is unknowable here, so neither count is published.
            break
        parts["shares_outstanding"] = sh
        if lag is not None:
            parts["shares_lag1y"] = (sh[0], lag)
        break

    # SUE on quarterly net income (split-invariant; the EPS form of Livnat &
    # Mendenhall 2006 mixes pre- and post-split scales in its history window).
    for c in C_NET_INCOME:
        q = derive_quarters(k.durations.get(c, {}))
        if q:
            got = sue_from_quarters(q, finite_statistics=finite_statistics)
            if got is not None:
                parts["sue"] = got
            break

    # The snapshot's fiscal period: the latest financial-statement period seen.
    statement_ends = [v[0] for v in parts.values()]
    if not statement_ends:
        return None, vals
    period_end = max(statement_ends)
    for name, (end, value) in parts.items():
        if (period_end - end).days <= 100:
            vals[name] = float(value)
    return period_end, vals


def parse_company_facts(doc: dict) -> list[Fact]:
    """Flatten a Company Facts JSON into Fact rows usable for PIT reconstruction."""
    wanted = set(C_EQUITY + C_EQUITY_NCI + C_ASSETS + C_LIABILITIES + C_NET_INCOME + C_REVENUE
                 + C_GROSS_PROFIT + C_COST_OF_REVENUE + C_CFO + C_OPERATING_INCOME + C_EPS
                 + C_SHARES_GAAP + C_WASO)
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


def load_id_bridge(warehouse: str, sr_ids: set[str], *, legacy_static_v1: bool = False
                   ) -> tuple[dict[str, str], dict[str, str]]:
    """SpiderRock security id -> 10-digit CIK, via the warehouse identifier history.

    Only ids whose warehouse security is a SEC-CIK security map; an id bridged to
    more than one CIK is ambiguous and reported, never guessed.
    """
    if not legacy_static_v1:
        raise ValueError("static CIK bridge requires explicit legacy-static-v1 reproduction")
    import duckdb  # local import: only explicit legacy export needs it

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


def _security_link_module():
    # The atx_db package facade eagerly imports warehouse loaders. Load this pure
    # repository module directly so the artifact export has no database dependency.
    name = "_atx_security_link_export"
    if name not in sys.modules:
        source = Path(__file__).resolve().parents[2] / "atx-db/src/atx_db/security_link.py"
        spec = importlib.util.spec_from_file_location(name, source)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return sys.modules[name]


def load_dated_id_bridge(directory: str, sr_ids: set[str]):
    links, manifest = _security_link_module().read_link_artifact(Path(directory))
    grouped = {sr: [] for sr in sr_ids}
    for link in links:
        if link.sr_id in grouped and link.live_eligible:
            grouped[link.sr_id].append(link)
    reasons = {sr: "no_qualified_dated_link" for sr, rows in grouped.items() if not rows}
    return grouped, reasons, manifest


def _ns(value: dt.date | dt.datetime) -> int:
    if isinstance(value, dt.datetime):
        delta = value.astimezone(dt.timezone.utc) - dt.datetime(1970, 1, 1, tzinfo=dt.timezone.utc)
        return (delta.days * 86400 + delta.seconds) * 10**9 + delta.microseconds * 1000
    return (value - dt.date(1970, 1, 1)).days * 86_400 * 10**9


INTERVAL_KEYS = ("sr_id", "owner_id", "link_id", "available_ns", "period_end_ns",
                 "identity_valid_from_ns", "identity_valid_to_ns", "link_available_ns",
                 "identity_only", "link_priority", "identity_retired_from_ns", "identity_retired_available_ns")


def project_dated_snapshots(link, snapshots: list[dict]) -> list[dict]:
    """Keep independent filing and identity clocks; expiry is consumer-enforced.

    A marker is emitted even without issuer facts so a later competing identity
    cannot leave the old issuer's values visible. Conflict markers never get facts.
    """
    if not link.live_eligible:
        raise ValueError("retrospective identity is audit-only; cannot produce live fundamental fields")
    base = {"sr_id": link.sr_id, "owner_id": link.owner_id, "link_id": link.link_id,
            "identity_valid_from_ns": _ns(link.valid_from),
            "identity_valid_to_ns": _ns(link.valid_to), "link_available_ns": _ns(link.available_at),
            "identity_retired_from_ns": _ns(link.retired_from) if link.retired_from else 2**63 - 1,
            "identity_retired_available_ns": _ns(link.retired_available_at) if link.retired_available_at else 2**63 - 1,
            "link_priority": int(link.method == "dated-override-v1")}
    marker = dict(base, available_ns=0, period_end_ns=0, identity_only=1,
                  **{name: math.nan for name in RAW_FIELDS})
    rows = [marker]
    if link.is_marker:
        return rows
    for snapshot in snapshots:
        if snapshot["available_date"] >= link.valid_to:
            continue
        rows.append(dict(base, available_ns=_ns(snapshot["available_date"]),
                         period_end_ns=_ns(snapshot["period_end"]), identity_only=0,
                         **{name: snapshot[name] for name in RAW_FIELDS}))
    return rows


def project_qualified_snapshots(link, snapshots: list[dict]) -> list[dict]:
    """V4 preserves D1 identity and the exact resolved filing nanosecond clock."""
    import pit_fundamental_clock as fc
    marker = project_dated_snapshots(link, [])[0]
    marker.update({key: "" if key == "clock_policy" else 0 for key in fc.CLOCK_KEYS})
    rows = [marker]
    if link.is_marker:
        return rows
    for snapshot in snapshots:
        if snapshot["available_ns"] >= _ns(link.valid_to):
            continue
        row = dict(marker, available_ns=snapshot["available_ns"],
                   period_end_ns=_ns(snapshot["period_end"]), identity_only=0)
        row.update({key: snapshot[key] for key in fc.CLOCK_KEYS})
        # An unverified fact vintage never becomes numerical input, even under
        # explicit modeled-clock admission. The audit retains the reason.
        row.update({name: snapshot[name] if snapshot["fact_vintage_qualified"] else math.nan
                    for name in RAW_FIELDS})
        rows.append(row)
    return rows


def write_qualified_points(path: Path, rows_by_sr: dict[str, list[dict]]) -> int:
    import pit_fundamental_clock as fc
    keys = INTERVAL_KEYS + fc.CLOCK_KEYS
    count = 0
    with path.open("w", encoding="utf-8", newline="") as stream:
        stream.write("ATX-FUNDAMENTAL-INTERVALS\t4\n")
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow(keys + RAW_FIELDS)
        for sr in sorted(rows_by_sr, key=lambda x: (len(x), x)):
            for row in sorted(rows_by_sr[sr], key=lambda r: (r["link_id"], r["available_ns"], r["period_end_ns"])):
                writer.writerow([row[key] for key in keys] +
                                [repr(row[f]) if math.isfinite(row[f]) else "" for f in RAW_FIELDS])
                count += 1
    return count


def write_interval_points(path: Path, rows_by_sr: dict[str, list[dict]]) -> int:
    with path.open("w", encoding="utf-8", newline="") as stream:
        stream.write("ATX-FUNDAMENTAL-INTERVALS\t3\n")
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow(INTERVAL_KEYS + RAW_FIELDS)
        count = 0
        for sr in sorted(rows_by_sr, key=lambda x: (len(x), x)):
            for row in sorted(rows_by_sr[sr], key=lambda r: (r["link_id"], r["available_ns"], r["period_end_ns"])):
                keys = [str(row[key]) for key in INTERVAL_KEYS]
                if any(any(c in key for c in '\t\r\n"') for key in keys):
                    raise ValueError("interval keys cannot contain TSV control/quote characters")
                writer.writerow(keys + [repr(row[f]) if math.isfinite(row[f]) else "" for f in RAW_FIELDS])
                count += 1
    return count


def align_interval_values(rows: list[dict], session_keys: list[int], lag_sessions: int,
                          cap_avail: int, cap_period: int) -> list[dict[str, float]]:
    """Small Python audit twin of the explicit v2 C++ decoder/aligner contract."""
    if lag_sessions < 0 or cap_avail <= 0 or cap_period <= 0:
        raise ValueError("invalid interval alignment policy")
    if any(a >= b for a, b in zip(session_keys, session_keys[1:])):
        raise ValueError("session keys must increase")
    visibility = [(max(bisect.bisect_right(session_keys, r["link_available_ns"]),
                       0 if r["identity_only"] else bisect.bisect_right(session_keys, r["available_ns"]) + lag_sessions), r)
                  for r in rows]
    output = []
    for t, key in enumerate(session_keys):
        eligible = [r for vis, r in visibility if vis <= t and r["identity_valid_from_ns"] <= key < r["identity_valid_to_ns"]
                    and not (key >= r["identity_retired_from_ns"] and key > r["identity_retired_available_ns"])]
        priority = max((r["link_priority"] for r in eligible), default=0)
        eligible = [r for r in eligible if r["link_priority"] == priority]
        values = {f: math.nan for f in RAW_FIELDS}
        if len({r["owner_id"] for r in eligible}) == 1:
            for f in RAW_FIELDS:
                candidates = [r for r in eligible if not r["identity_only"] and math.isfinite(r[f])]
                if candidates:
                    best = max(candidates, key=lambda r: (r["period_end_ns"], r["available_ns"]))
                    if (key - best["available_ns"] <= cap_avail * 86400 * 10**9 and
                            key - best["period_end_ns"] <= cap_period * 86400 * 10**9):
                        values[f] = best[f]
        output.append(values)
    return output


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
    ap.add_argument("--warehouse", help="explicit legacy-static-v1 warehouse (opened read-only)")
    ap.add_argument("--bridge-rule", choices=("dated-links-v2", "legacy-static-v1"), default="dated-links-v2")
    ap.add_argument("--security-links", help="hash-bound D1 security-link artifact directory")
    ap.add_argument("--filing-clock-rule", choices=("legacy-filed-day-v1", "acceptance-v2"),
                    default="legacy-filed-day-v1")
    ap.add_argument("--filing-clocks", help="sealed issuer/accession filing-clock JSON projection")
    ap.add_argument("--filing-clocks-sealed-manifest")
    ap.add_argument("--acceptance-delay-seconds", type=int,
                    help="explicit modeled dissemination lag when observed publication is absent")
    ap.add_argument("--companyfacts-sealed-manifest", help="required pre-2020 source projection receipt for v2")
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
    if seal > dt.date(2020, 1, 1):
        raise SystemExit("seal may not extend beyond 2020-01-01")
    strict = args.bridge_rule == "dated-links-v2"
    accepted_clock = args.filing_clock_rule == "acceptance-v2"
    filing_clocks, filing_clock_proof = {}, {}
    if accepted_clock:
        if not strict or not args.filing_clocks or not args.filing_clocks_sealed_manifest or args.acceptance_delay_seconds is None:
            raise SystemExit("acceptance-v2 requires dated links, sealed filing clocks and explicit acceptance-delay-seconds")
        import pit_fundamental_clock as fc
        if not 0 <= args.acceptance_delay_seconds <= 7 * 86400:
            raise SystemExit("invalid modeled dissemination delay")
        filing_clocks, filing_clock_proof = fc.load_projection(
            Path(args.filing_clocks), Path(args.filing_clocks_sealed_manifest), seal)
    elif any(value is not None for value in (args.filing_clocks, args.filing_clocks_sealed_manifest,
                                              args.acceptance_delay_seconds)):
        raise SystemExit("filing-clock inputs require acceptance-v2")
    if strict:
        if not args.security_links or not args.companyfacts_sealed_manifest:
            raise SystemExit("dated-links-v2 requires --security-links and --companyfacts-sealed-manifest")
        proof = json.loads(Path(args.companyfacts_sealed_manifest).read_text(encoding="utf-8"))
        if (proof.get("schema") != "atx.sec-companyfacts-sealed/v1" or
                dt.date.fromisoformat(proof["seal_exclusive"]) > seal or
                sha256_file(Path(args.companyfacts)) != proof["payload_sha256"]):
            raise SystemExit("unqualified sealed Company Facts projection")
        if accepted_clock and filing_clock_proof["facts_payload_sha256"] != proof["payload_sha256"]:
            raise SystemExit("filing-clock vintage evidence is bound to another Company Facts projection")
    elif not args.warehouse:
        raise SystemExit("legacy-static-v1 requires --warehouse")
    contexts = load_contexts(args.contexts)
    if any(any(key >= _ns(seal) for key in context.session_keys) for context in contexts):
        raise SystemExit("context reaches the sealed era")
    sr_ids = sorted({sr for c in contexts for sr in c.instrument_ids})
    if strict:
        mapping, unmapped, bridge_manifest = load_dated_id_bridge(args.security_links, set(sr_ids))
    else:
        mapping, unmapped = load_id_bridge(args.warehouse, set(sr_ids), legacy_static_v1=True)

    zf = zipfile.ZipFile(args.companyfacts)
    members = set(zf.namelist())
    rows_by_sr: dict[str, list[dict]] = {}
    lag_by_form: dict[str, list[int]] = {}
    ciks_missing_facts = 0
    by_cik: dict[str, list] = {}
    true_shares, clock_audit, rejected_facts = [], [], []
    total_v4_rows = 0
    if strict:
        for sr, links in mapping.items():
            for link in links:
                rows_by_sr.setdefault(sr, []).extend(
                    project_qualified_snapshots(link, []) if accepted_clock else project_dated_snapshots(link, []))
                total_v4_rows += 1
                if accepted_clock and total_v4_rows > 1_000_000:
                    raise ValueError("acceptance-v2 identity row budget")
                if not link.is_marker:
                    by_cik.setdefault(link.cik, []).append(link)
    else:
        for sr, cik in mapping.items():
            by_cik.setdefault(cik, []).append(sr)
    for n, (cik, srs) in enumerate(sorted(by_cik.items())):
        member = f"CIK{cik}.json"
        if member not in members:
            ciks_missing_facts += 1
            for sr in srs:
                if strict:
                    sr = sr.sr_id
                unmapped[sr] = "cik_absent_from_companyfacts"
            continue
        if accepted_clock and zf.getinfo(member).file_size > 32 * 1024 * 1024:
            raise ValueError("acceptance-v2 per-company projection exceeds 32 MiB")
        doc = json.loads(zf.read(member))
        if strict:
            try:
                if _security_link_module().cik_key(doc.get("cik", "")) != cik:
                    raise ValueError("Company Facts body issuer does not match dated link CIK")
            except ValueError:
                zf.close()
                raise
        if accepted_clock:
            accounting = set(C_EQUITY + C_EQUITY_NCI + C_ASSETS + C_LIABILITIES + C_NET_INCOME +
                C_REVENUE + C_GROSS_PROFIT + C_COST_OF_REVENUE + C_CFO + C_OPERATING_INCOME + C_EPS +
                C_SHARES_GAAP + C_WASO)
            facts, rejected = fc.parse_facts(doc, accounting, EVENT_FORMS)
            snaps, shares, clocks = fc.snapshot_events(facts, cik, filing_clocks, seal,
                args.acceptance_delay_seconds, knowledge_factory=Knowledge, snapshot_fn=snapshot,
                event_forms=EVENT_FORMS)
            if max(len(true_shares) + len(shares), len(clock_audit) + len(clocks),
                   len(rejected_facts) + len(rejected)) > 1_000_000:
                raise ValueError("acceptance-v2 audit row budget")
            true_shares.extend(shares); clock_audit.extend(clocks)
            rejected_facts.extend(dict(cik=cik, **item) for item in rejected)
        else:
            snaps = company_snapshots(parse_company_facts(doc), seal)
        for s in snaps:
            lag_by_form.setdefault(s["form"], []).append((s["filed"] - s["period_end"]).days)
        for sr in srs:
            if strict:
                projected = (project_qualified_snapshots(sr, snaps) if accepted_clock else
                             project_dated_snapshots(sr, snaps))[1:]
                total_v4_rows += len(projected)
                if accepted_clock and total_v4_rows > 1_000_000:
                    raise ValueError("acceptance-v2 output row budget")
                rows_by_sr[sr.sr_id].extend(projected)
            else:
                rows_by_sr[sr] = [dict(s, sr_id=sr, cik=cik) for s in snaps]
        if n % 100 == 0:
            print(f"[export] {n}/{len(by_cik)} companies", file=sys.stderr)

    out.mkdir(parents=True)
    zf.close()
    points = out / ("points.interval-v4.tsv" if accepted_clock else "points.interval-v3.tsv" if strict else "points.csv")
    n_rows = 0
    if strict:
        n_rows = (write_qualified_points if accepted_clock else write_interval_points)(points, rows_by_sr)
    else:
        with open(points, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(KEY_COLUMNS + RAW_FIELDS)
            for sr in sorted(rows_by_sr, key=lambda s: (len(s), s)):
                for r in rows_by_sr[sr]:
                    w.writerow([r["sr_id"], r["cik"], r["filed"].isoformat(), r["available_date"].isoformat(),
                                r["period_end"].isoformat(), r["form"], r["accn"]]
                               + [("" if not math.isfinite(r[f]) else repr(r[f])) for f in RAW_FIELDS])
                    n_rows += 1

    if strict:
        audits = []
        for context in contexts:
            covered = {f: 0 for f in RAW_FIELDS}
            for sr in context.instrument_ids:
                source_rows = rows_by_sr.get(sr, [])
                if accepted_clock:
                    source_rows = [dict(r, **{f: math.nan for f in RAW_FIELDS})
                        if not r["identity_only"] and not (r["clock_kind"] == 1 and
                            r["fact_vintage_qualified"] and r["knowledge_clock_qualified"]) else r
                        for r in source_rows]
                aligned = align_interval_values(source_rows, context.session_keys,
                    args.lag_sessions, args.max_days_since_available, args.max_days_since_period_end)
                for row in aligned:
                    for f in RAW_FIELDS:
                        covered[f] += math.isfinite(row[f])
            audits.append({"context": context.name, "scope": "all_axis_cells_not_operating_company_gate",
                           "finite_cells": covered, "plan_coverage_qualified": False})
    else:
        audits = [audit_context(c, rows_by_sr, args.lag_sessions, args.max_days_since_available,
                                args.max_days_since_period_end) for c in contexts]
    unmapped_counts: dict[str, int] = {}
    for reason in unmapped.values():
        unmapped_counts[reason] = unmapped_counts.get(reason, 0) + 1
    audit = {
        "schema": "atx.fundamental-fields-availability/v1",
        "panel_securities": len(sr_ids),
        "securities_with_snapshots": sum(1 for v in rows_by_sr.values()
                                         if any(not r.get("identity_only", False) for r in v)),
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
        "schema": "atx.fundamental-fields-export/v3" if strict else "atx.fundamental-fields-export/v1",
        "tool": TOOL_VERSION,
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "clock_policy": CLOCK_POLICY,
        "identity_rule": args.bridge_rule,
        "seal_exclusive": seal.isoformat(),
        "inputs": {
            "companyfacts": {"path": args.companyfacts, "size": os.path.getsize(args.companyfacts),
                             "sha256": sha256_file(Path(args.companyfacts))},
            "warehouse_bridge": None if strict else {"path": args.warehouse, "size": os.path.getsize(args.warehouse),
                                 "table": "security_identifier_history",
                                 "id_type": "TBLTICKERHISTORY_SECURITY_ID"},
            "contexts": [c.name for c in contexts],
        },
        "columns": {"keys": list(INTERVAL_KEYS if strict else KEY_COLUMNS), "raw_fields": list(RAW_FIELDS)},
        "consumer": ("atx/engine/data/fundamental_fields_artifact.hpp::decode_interval_points" if strict
                     else "atx/engine/data/fundamental_fields.hpp (legacy static identity)"),
        "share_pair_plausibility": {"rule": SHARE_PAIR_RULE,
                                    "knowledge_states_rejected": SHARE_PAIRS_REJECTED[0]},
        "id_bridge_caveat": ("TBLTICKERHISTORY_SECURITY_ID -> SEC-CIK-* links come from matching the "
                             "vendor's CURRENT ticker against the current SEC company_tickers file; a "
                             "name is bridged only if its current ticker is still SEC-listed, so every "
                             "field is survivor-conditioned (look-ahead selection), not point-in-time."),
        "outputs": {points.name: {"rows": n_rows, "sha256": sha256_file(points)}},
    }
    if strict:
        manifest["inputs"]["security_links"] = {
            "path": args.security_links, "schema": bridge_manifest["schema"],
            "manifest_sha256": sha256_file(Path(args.security_links) / "manifest.json")}
        manifest["inputs"]["contexts"] = [{"path": str(c.path), "name": c.name,
            "manifest_sha256": sha256_file(c.path / "context.bin.manifest.json")} for c in contexts]
        manifest["inputs"]["companyfacts_sealed_manifest"] = {
            "path": args.companyfacts_sealed_manifest,
            "sha256": sha256_file(Path(args.companyfacts_sealed_manifest))}
        manifest["id_bridge_caveat"] = (
            "Dated vendor-line identity with independent proof clocks and exclusive expiry; "
            "Company Facts filing-date arithmetic remains modeled legacy policy, not a verified acceptance clock.")
        manifest["plan_coverage_qualified"] = False
    for filename in ("availability_audit.json", "unmapped_securities.csv"):
        manifest["outputs"][filename] = {"sha256": sha256_file(out / filename)}
    part = out / "manifest.json.part"
    if accepted_clock:
        share_path = out / "shares.entity-v1.jsonl"
        with share_path.open("w", encoding="utf-8", newline="") as stream:
            for row in sorted(true_shares, key=lambda r: (r["cik"], r["available_ns"], r["accession"], r["shares_observation_date"])):
                stream.write(json.dumps(row, sort_keys=True, allow_nan=False) + "\n")
        audit_path = out / "filing_clock_audit.json"
        audit_path.write_text(json.dumps({"schema": "atx.filing-clock-audit/v2", "events": clock_audit,
            "rejected_facts": rejected_facts, "qualification_scope": "all active retained accounting cells; conservative"},
            indent=2, allow_nan=False), encoding="utf-8")
        manifest.update(schema="atx.fundamental-fields-export/v4", tool="fundamental-fields-export-v4",
            clock_policy="accession-resolved-ns-v2", filing_clock_rule=args.filing_clock_rule,
            acceptance_delay_seconds=args.acceptance_delay_seconds,
            accounting_lookback_days=fc.ACTIVE_LOOKBACK_DAYS,
            qualification_scope="all retained accounting cells within declared lookback; conservative",
            default_admission="observed-public AND qualified active knowledge/vintage; strict less-than decision",
            legacy_clock_caveat="explicit legacy-filed-day-v1 preserves midnight(filed+1), despite historical +46h label",
            true_shares_scope="DEI entity instant; exact duplicates deduped; conflicts withheld; not line shares or split-rebased cap",
            cap_output="unavailable: requires separate dated class/split/price evidence",
            consumer="data/fundamental_clock_artifact.hpp::decode_qualified_interval_points")
        manifest["columns"]["keys"] = list(INTERVAL_KEYS + fc.CLOCK_KEYS)
        manifest["inputs"]["filing_clocks"] = {
            "path": args.filing_clocks, "payload_sha256": filing_clock_proof["payload_sha256"],
            "sealed_manifest_sha256": sha256_file(Path(args.filing_clocks_sealed_manifest)),
            "facts_payload_sha256": filing_clock_proof["facts_payload_sha256"]}
        manifest["outputs"][share_path.name] = {"rows": len(true_shares), "sha256": sha256_file(share_path)}
        manifest["outputs"][audit_path.name] = {"sha256": sha256_file(audit_path)}
        manifest["identity_qualification"] = "D1 independently clocked dated identity; filing/public/vintage admission V4"
        manifest["id_bridge_caveat"] = "Dated identity; clock and vintage evidence are source-declared, not independently authenticated by this exporter"
        manifest["recipe"] = {"rule": "acceptance-v2", "acceptance_delay_seconds": args.acceptance_delay_seconds,
            "accounting_lookback_days": fc.ACTIVE_LOOKBACK_DAYS,
            "clock_rows_sha256": manifest["outputs"][audit_path.name]["sha256"],
            "clock_input_sha256": filing_clock_proof["payload_sha256"],
            "companyfacts_sha256": manifest["inputs"]["companyfacts"]["sha256"],
            "admission": manifest["default_admission"], "identity_rule": args.bridge_rule}
        manifest["recipe_sha256"] = hashlib.sha256(json.dumps(manifest["recipe"],
            sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()
    part.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    part.rename(out / "manifest.json")
    print(json.dumps({"rows": n_rows, "mapped": len(mapping), "panel_securities": len(sr_ids)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
