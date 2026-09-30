"""Bounded producer of point-in-time fundamental event rows (mega-alpha T20, library v4 data).

Output contract: ``fundamental_events_schema.md`` in this directory (``atx.fundamental-events/v1``).
Rulings: v4 pre-registration section R2 (FSDS ``accepted_utc`` clock with a labelled FC1 fallback,
latest-clock-wins restatements, a seal, values modeled/unaccepted). The seal is the research seal of
``research_window.py`` (``SEAL_DATE``): nothing filed or accepted on or after it is read.

Sources (read-only, pinned by SHA-256):

* CF-R: the accepted CompanyFacts extraction (``batch-NNNN.parquet`` + ``manifest.json``), one CIK range per
  batch; only us-gaap facts of the periodic forms, in USD or shares, filed before the seal are read.
* FSDS v2 SUB quarters 2009q2..``LAST_SUB_QUARTER`` (the quarter holding the day before the seal;
  ``fsds-staging-manifest.json`` pins each file): the accession acceptance clock and the SIC code as of each
  filing. No later quarter is opened.
* The CIK scope list (T19 identity bridge; text, CSV or parquet with a ``cik`` column).
* Concept map: ``atx_db.statement_map_seed`` (canonical metric -> concepts by ``concept_priority``, with the
  total-over-component ``PRECEDENCE_OVERRIDES``), imported
  from this repository's ``atx-db/src`` and pinned by code hash; ``sue_from_quarters`` and
  ``plausible_share_pair`` are reused from ``export_fundamental_fields.py`` (also pinned).

Stages (each invocation cooperatively bounded by ``--max-seconds`` / ``--max-rss-mib``)::

    python build_fundamental_events.py prepare --out OUT --cik-list CIKS --companyfacts-dir CF --fsds-dir FSDS
    python build_fundamental_events.py events --out OUT --batches 0-20      # repeat per chunk; resumable
    python build_fundamental_events.py finalize --out OUT                   # manifest.json published last

``events`` skips a batch whose receipt verifies (same run plan, same output SHA-256) and refuses one whose
receipt does not. On a budget stop it exits 3 after the last completed batch; re-running the same command
resumes. Exit codes: 0 done, 2 usage/verification error, 3 budget stop (partial, resumable).
"""
from __future__ import annotations

import argparse
import bisect
import csv
import datetime as dt
import hashlib
import json
import math
import os
import sys
import time
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parents[1]
ATX_DB_SRC = REPO_ROOT / "atx-db" / "src"
for _p in (str(ATX_DB_SRC), str(TOOLS_DIR)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import export_fundamental_fields as eff  # noqa: E402  pure period arithmetic (sue, share-pair rule)
import research_window as rw  # noqa: E402  the research window (the seal)
from atx_db import statement_map_seed as sms  # noqa: E402  read-only concept map

SCHEMA = "atx.fundamental-events/v1"
RUN_SCHEMA = "atx.fundamental-events.run/v1"
BATCH_RECEIPT_SCHEMA = "atx.fundamental-events.batch-receipt/v1"
PREPARE_RECEIPT_SCHEMA = "atx.fundamental-events.prepare-receipt/v1"
SCOPE_FILE = "cik_scope.txt"
TOOL_VERSION = "fundamental-events-v1"

EPOCH = dt.date(1970, 1, 1)
SEAL = rw.SEAL  # first sealed date (research_window.py)
EMIT_FROM = dt.date(2014, 6, 1)
US_PER_DAY = 86_400_000_000
FC1_OFFSET_US = 46 * 3_600_000_000
FIRST_SUB_QUARTER = "2009q2"
LAST_SUB_QUARTER = "{}q{}".format(*rw.last_quarter_before_seal())  # the seal: no later SUB quarter is opened
DEFAULT_CF_MANIFEST_SHA256 = "50e018e1c26046f3eb3ec27d2f246c8492e60baddbc911ebfda50320f24ac186"
DEFAULT_FSDS_MANIFEST_SHA256 = "2cad6134efd312d7ad9ca84bbc274fdf360e10e581b38aa88a29f1040bebc628"

EVENT_FORMS = ("10-K", "10-K/A", "10-Q", "10-Q/A", "10-KT", "10-KT/A", "10-QT", "10-QT/A",
               "20-F", "20-F/A", "40-F", "40-F/A")
QUARTERLY_FORMS = frozenset(("10-Q", "10-Q/A", "10-QT", "10-QT/A"))
BASIS_FSDS = "fsds_accepted_utc"
BASIS_FC1 = "cf_fc1"

# Canonical metrics of the atx-db statement map used here (industry template ALL).
INSTANT_METRICS = (
    "total_assets", "total_liabilities", "stockholders_equity", "equity_incl_minority", "minority_int_bs",
    "pref_stock", "cash_and_st_investments", "cash", "st_investments", "cash_st_inv", "st_debt", "lt_debt",
    "current_assets", "current_liabilities",
)
DURATION_METRICS = (
    "revenue", "cogs", "gross_profit", "operating_income", "net_income", "operating_cash_flow",
    "cfo_continuing", "capital_expenditures", "rd_expense", "common_div_paid", "dividends_paid",
    "share_repurchases", "stock_issuance", "income_tax", "shares_diluted_avg", "shares_basic_avg",
)
METRICS = INSTANT_METRICS + DURATION_METRICS
M = {name: i for i, name in enumerate(METRICS)}
N_INSTANT = len(INSTANT_METRICS)
UNIT_OF = {"monetary": "USD", "shares": "shares"}
CORE_METRICS = frozenset(M[n] for n in ("total_assets", "stockholders_equity", "net_income", "revenue",
                                        "operating_cash_flow"))
# Total-over-component precedence (T20 fix round 1). The seed ranks some components ahead of the taxonomy total:
# ASC 606 contract revenue (excludes lease, interest, derivative and insurance revenue) ahead of `Revenues`, `Cash`
# ahead of cash and cash equivalents, and current maturities of long-term debt ahead of total current debt. The
# listed totals move to the front of their metric (in this order); every other concept keeps the seed order, so a
# component remains the fallback when the total is not tagged for a period.
PRECEDENCE_OVERRIDES = {
    "revenue": ("Revenues",),
    "cash": ("CashAndCashEquivalentsAtCarryingValue",),
    "st_debt": ("DebtCurrent",),
}
RFC_CONCEPTS = ("RevenueFromContractWithCustomerExcludingAssessedTax",
                "RevenueFromContractWithCustomerIncludingAssessedTax")

ITEMS = (
    "be", "at", "at_lag4", "lt", "che", "debt", "sale_ttm", "gp_ttm", "oi_ttm", "ni_ttm", "ni_q", "ni_q_lag4",
    "be_lag1q", "be_lag1q_lag4", "cfo_ttm", "capx_ttm", "xrd_ttm", "dvc_ttm", "prstkc_ttm", "sstk_ttm", "txt_q",
    "txt_q_lag4", "shrs_q", "shrs_q_lag4", "noa", "noa_lag4", "sue", "fscore",
)
ITEM_UNITS = {name: "USD" for name in ITEMS}
ITEM_UNITS.update({"shrs_q": "shares", "shrs_q_lag4": "shares", "sue": "1", "fscore": "count_0_9"})
ZERO_FILL_ITEMS = ("debt", "noa", "noa_lag4", "fscore", "dvc_ttm", "prstkc_ttm", "sstk_ttm")

# Period arithmetic (days). Quarters 80-100 d, fiscal years 350-380 d (52/53-week years included).
Q_MIN, Q_MAX, Y_MIN, Y_MAX = 80, 100, 350, 380
TOL_ANCHOR = 7
TOL_LAG = 20
TOL_SAME_DATE = 3
TOL_SNAP = 10
LAG4, LAG1Q, LAG8 = 365, 91, 730
SUE_WINDOW_DAYS = 3 * 365 + 30
STALE_QUARTERLY, STALE_ANNUAL_ONLY, ANNUAL_ONLY_LOOKBACK = 200, 400, 400
NAN = float("nan")

EVENT_SCHEMA = pa.schema(
    [
        ("cik", pa.int64()),
        ("accession", pa.string()),
        ("accepted_utc", pa.timestamp("us", tz="UTC")),
        ("clock_basis", pa.string()),
        ("filed", pa.date32()),
        ("form", pa.string()),
        ("report_period", pa.date32()),
        ("period_end", pa.date32()),
        ("fiscal_year", pa.int32()),
        ("fiscal_period", pa.string()),
        ("fiscal_year_end", pa.string()),
        ("staleness_days", pa.int32()),
    ]
    + [(name, pa.float64()) for name in ITEMS]
    + [("zero_filled", pa.string()), ("n_facts", pa.int32()), ("n_restated", pa.int32())]
)
SIC_SCHEMA = pa.schema(
    [
        ("cik", pa.int64()),
        ("accession", pa.string()),
        ("accepted_utc", pa.timestamp("us", tz="UTC")),
        ("clock_basis", pa.string()),
        ("filed", pa.date32()),
        ("form", pa.string()),
        ("sic", pa.int32()),
    ]
)
CLOCK_SCHEMA = pa.schema(
    [
        ("adsh", pa.string()),
        ("cik", pa.int64()),
        ("accepted_us", pa.int64()),
        ("clock_basis", pa.string()),
        ("filed", pa.int32()),
        ("form", pa.string()),
        ("period", pa.int32()),
        ("fy", pa.int32()),
        ("fp", pa.string()),
        ("fye", pa.string()),
        ("sic", pa.int32()),
    ]
)
CF_COLUMNS = ["cik", "taxonomy", "concept", "unit", "period_start", "period_end", "filed_date", "fiscal_year",
              "fiscal_period", "form", "accession_number", "value"]
SUB_COLUMNS = ["adsh", "cik", "sic", "form", "period", "fy", "fp", "filed", "accepted_utc", "fye"]


class UsageError(Exception):
    """Invalid input, pin mismatch or refused resume (exit 2)."""


class BudgetStop(Exception):
    """Cooperative budget stop between batches (exit 3, resumable)."""


# ---------------------------------------------------------------------------
# Small utilities
# ---------------------------------------------------------------------------

def canonical(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def day_of(value: dt.date) -> int:
    return (value - EPOCH).days


def date_of(day: int) -> dt.date:
    return EPOCH + dt.timedelta(days=int(day))


SEAL_US = day_of(SEAL) * US_PER_DAY
EMIT_FROM_US = day_of(EMIT_FROM) * US_PER_DAY


def write_json_atomic(path: Path, value) -> bytes:
    content = (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")
    tmp = path.with_name("." + path.name + ".tmp")
    with tmp.open("wb") as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    return content


def publish_last(path: Path, value) -> bytes:
    """Exclusive publish: a reader sees no file or its complete fsynced bytes; never overwrites."""
    content = (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")
    pending = path.with_name("." + path.name + ".pending")
    if pending.exists():
        pending.unlink()
    with pending.open("xb") as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
    os.link(pending, path)
    pending.unlink()
    return content


def write_parquet_atomic(table: pa.Table, path: Path) -> None:
    tmp = path.with_name("." + path.name + ".tmp")
    pq.write_table(table, tmp, compression="zstd", compression_level=3, use_dictionary=True,
                   write_statistics=True, row_group_size=131072)
    with tmp.open("r+b") as f:
        os.fsync(f.fileno())
    os.replace(tmp, path)


class Budget:
    """Cooperative time/RSS guard polled between batches (not a kernel quota)."""

    def __init__(self, max_seconds: float, max_rss_mib: int):
        if not 10 <= max_seconds <= 600 or not 256 <= max_rss_mib <= 4096:
            raise UsageError("invalid budget: --max-seconds in [10, 600], --max-rss-mib in [256, 4096]")
        self.start = time.monotonic()
        self.max_seconds = float(max_seconds)
        self.max_rss = int(max_rss_mib) << 20
        self.peak = 0
        try:
            import psutil
            self._proc = psutil.Process()
        except ImportError:  # pragma: no cover - psutil ships with the admitted interpreter
            self._proc = None

    def rss(self) -> int:
        value = int(self._proc.memory_info().rss) if self._proc else 0
        self.peak = max(self.peak, value)
        return value

    def elapsed(self) -> float:
        return time.monotonic() - self.start

    def admit(self, projected_seconds: float, stage: str) -> None:
        if self.elapsed() + projected_seconds > self.max_seconds:
            raise BudgetStop(f"time budget: {self.elapsed():.1f}s used, next step ~{projected_seconds:.1f}s "
                             f"exceeds --max-seconds {self.max_seconds:.0f} before {stage}")
        if self.rss() > self.max_rss:
            raise BudgetStop(f"RSS {self.rss() >> 20} MiB exceeds --max-rss-mib before {stage}")


# ---------------------------------------------------------------------------
# Concept map (atx-db statement map seed, read-only)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class ConceptMap:
    concept_to: dict  # concept -> (metric index, rank, unit)
    by_metric: dict   # metric name -> [[concept, concept_priority], ...] in rank order
    sha256: str


def load_concept_map(rows=None) -> ConceptMap:
    """Resolve the 30 canonical metrics to us-gaap concepts in ``concept_priority`` order.

    Ties in priority are broken by concept name so ranks are total and deterministic. ``PRECEDENCE_OVERRIDES``
    then moves a metric's taxonomy total ahead of the seed's component concepts (all other concepts keep the seed
    order), so a total and a component tagged for the same period never resolve to the component.
    """
    rows = sms.default_statement_map_rows() if rows is None else rows
    grouped: dict[str, list] = {name: [] for name in METRICS}
    for row in rows:
        if (row.canonical_metric in grouped and row.industry_template == "ALL" and row.is_active
                and not row.is_derived and row.taxonomy == "us-gaap"):
            want = "instant" if M[row.canonical_metric] < N_INSTANT else "duration"
            if row.period_type != want or row.unit_type not in UNIT_OF:
                raise UsageError(f"statement map row {row.concept}: unexpected {row.period_type}/{row.unit_type}")
            grouped[row.canonical_metric].append((row.concept_priority, row.concept, UNIT_OF[row.unit_type]))
    concept_to: dict[str, tuple[int, int, str]] = {}
    by_metric: dict[str, list] = {}
    for name in METRICS:
        entries = sorted(set(grouped[name]))
        if not entries:
            raise UsageError(f"statement map has no concept for metric {name}")
        promoted = PRECEDENCE_OVERRIDES.get(name, ())
        missing = [c for c in promoted if c not in {e[1] for e in entries}]
        if missing:
            raise UsageError(f"precedence override for {name}: {missing} not in the statement map")
        entries = ([e for c in promoted for e in entries if e[1] == c]
                   + [e for e in entries if e[1] not in promoted])
        by_metric[name] = [[concept, priority] for priority, concept, _unit in entries]
        for rank, (_priority, concept, unit) in enumerate(entries):
            if concept in concept_to:
                raise UsageError(f"concept {concept} maps to two metrics")
            concept_to[concept] = (M[name], rank, unit)
    digest = hashlib.sha256(canonical(by_metric).encode("utf-8")).hexdigest()
    return ConceptMap(concept_to, by_metric, digest)


def code_hashes() -> dict[str, str]:
    seed_module = Path(sms.__file__).resolve()
    if seed_module.parent != (ATX_DB_SRC / "atx_db").resolve():
        raise UsageError(f"atx_db imported from {seed_module}, not this repository's {ATX_DB_SRC}")
    return {
        "atx-engine/tools/build_fundamental_events.py": sha256_file(Path(__file__).resolve()),
        "atx-engine/tools/export_fundamental_fields.py": sha256_file(Path(eff.__file__).resolve()),
        "atx-db/src/atx_db/statement_map_seed.py": sha256_file(seed_module),
        "atx-db/src/atx_db/seeds/statement_map.csv": sha256_file(Path(sms.STATEMENT_MAP_SEED_PATH)),
    }


# ---------------------------------------------------------------------------
# Knowledge state: canonical metric series, merged by concept rank, latest clock wins
# ---------------------------------------------------------------------------

def _within(ends: list, target: int, tol: int) -> list:
    lo = bisect.bisect_left(ends, target - tol)
    hi = bisect.bisect_right(ends, target + tol)
    found = ends[lo:hi]
    if len(found) > 1:
        found.sort(key=lambda e: (abs(e - target), e))
    return found


def _same_value(a: float, b: float) -> bool:
    return a == b or abs(a - b) <= 1e-9 * max(1.0, abs(a), abs(b))


PUT_NONE, PUT_RESTATED, PUT_SHADOWED = 0, 1, 2


def _merge(old, rank: int, value: float, seq: int):
    """Merge rule for one metric key; ``old`` = (rank, value, seq) or None. Returns (new entry or None, code).

    A better-ranked concept replaces; the same rank is replaced by the later accession (restated when the value
    differs; an equal value re-confirms and advances ``seq``); a worse rank is ignored, and counted as shadowed
    when an earlier accession's better-ranked value differs from it.
    """
    if old is None or rank < old[0]:
        return (rank, value, seq), PUT_NONE
    if rank == old[0]:
        return (rank, value, seq), (PUT_NONE if _same_value(value, old[1]) else PUT_RESTATED)
    return None, (PUT_SHADOWED if old[2] < seq and not _same_value(value, old[1]) else PUT_NONE)


class InstantSeries:
    __slots__ = ("vals", "ends")

    def __init__(self):
        self.vals: dict[int, tuple[int, float, int]] = {}  # end -> (rank, value, writer seq)
        self.ends: list[int] = []

    def put(self, end: int, rank: int, value: float, seq: int = 0) -> int:
        old = self.vals.get(end)
        new, code = _merge(old, rank, value, seq)
        if new is not None:
            if old is None:
                bisect.insort(self.ends, end)
            self.vals[end] = new
        return code

    def nearest(self, target: int, tol: int):
        found = _within(self.ends, target, tol)
        return found[0] if found else None

    def get(self, target: int, tol: int):
        end = self.nearest(target, tol)
        return None if end is None else self.vals[end][1]


class DurationSeries:
    __slots__ = ("vals", "by_end", "ends", "by_start", "starts")

    def __init__(self):
        self.vals: dict[tuple[int, int], tuple[int, float, int]] = {}  # (start, end) -> (rank, value, seq)
        self.by_end: dict[int, list[int]] = {}
        self.ends: list[int] = []
        self.by_start: dict[int, list[int]] = {}
        self.starts: list[int] = []

    def put(self, start: int, end: int, rank: int, value: float, seq: int = 0) -> int:
        key = (start, end)
        old = self.vals.get(key)
        new, code = _merge(old, rank, value, seq)
        if new is None:
            return code
        if old is None:
            if end not in self.by_end:
                self.by_end[end] = []
                bisect.insort(self.ends, end)
            self.by_end[end].append(start)
            if start not in self.by_start:
                self.by_start[start] = []
                bisect.insort(self.starts, start)
            self.by_start[start].append(end)
        self.vals[key] = new
        return code

    def value(self, start: int, end: int) -> float:
        return self.vals[(start, end)][1]

    def seq(self, start: int, end: int) -> int:
        return self.vals[(start, end)][2]

    def direct(self, target: int, tol: int, lo: int, hi: int, mid: int):
        """(start, end, value) of the fact ending nearest ``target`` whose duration is in [lo, hi]."""
        for end in _within(self.ends, target, tol):
            best = None
            for start in self.by_end[end]:
                days = end - start + 1
                if lo <= days <= hi:
                    cand = (abs(days - mid), -start)
                    if best is None or cand < best:
                        best = cand
            if best is not None:
                start = -best[1]
                return start, end, self.value(start, end)
        return None

    def quarter(self, target: int, tol: int):
        """Discrete fiscal quarter ending near ``target``: direct 80-100 d fact, else same-start YTD difference."""
        for end in _within(self.ends, target, tol):
            starts = self.by_end[end]
            best = None
            for start in starts:
                days = end - start + 1
                if Q_MIN <= days <= Q_MAX:
                    cand = (abs(days - 91), -start)
                    if best is None or cand < best:
                        best = cand
            if best is not None:
                start = -best[1]
                return start, end, self.value(start, end)
            for start in sorted(starts, reverse=True):  # shortest cumulative period first
                days = end - start + 1
                if days <= Q_MAX or days > Y_MAX:
                    continue
                best0 = None
                for end0 in self.by_start[start]:
                    gap = end - end0
                    if Q_MIN <= gap <= Q_MAX and end0 - start + 1 >= Q_MIN:
                        cand = (abs(gap - 91), end0)
                        if best0 is None or cand < best0:
                            best0 = cand
                if best0 is not None:
                    end0 = best0[1]
                    return end0 + 1, end, self.value(start, end) - self.value(start, end0)
        return None

    def ttm(self, target: int, tol: int):
        """Trailing twelve months ending near ``target``: FY fact; YTD + prior FY - prior YTD; 4 quarters."""
        for end in _within(self.ends, target, tol):
            got = self.direct(end, 0, Y_MIN, Y_MAX, 365)
            if got is not None:
                return got[2]
            for start in sorted(self.by_end[end], reverse=True):
                days = end - start + 1
                if not Q_MIN <= days < Y_MIN:
                    continue
                fy = self.direct(start - 1, TOL_SNAP, Y_MIN, Y_MAX, 365)
                if fy is None:
                    continue
                prior = self._prior_ytd(fy[0], end - LAG4, days)
                if prior is not None:
                    return self.value(start, end) + fy[2] - prior
            q0 = self.quarter(end, 0)
            if q0 is not None:
                total, start, ok = q0[2], q0[0], True
                for _ in range(3):
                    prev = self.quarter(start - 1, TOL_SNAP)
                    if prev is None:
                        ok = False
                        break
                    total += prev[2]
                    start = prev[0]
                if ok:
                    return total
        return None

    def _prior_ytd(self, fy_start: int, target_end: int, days: int):
        best = None
        for start in _within(self.starts, fy_start, TOL_SNAP):
            for end in self.by_start[start]:
                if abs(end - target_end) <= TOL_LAG and abs((end - start + 1) - days) <= TOL_LAG:
                    cand = (abs(start - fy_start) + abs(end - target_end), start, end)
                    if best is None or cand < best:
                        best = cand
        return None if best is None else self.value(best[1], best[2])


class CikState:
    __slots__ = ("series", "first_q", "first_q_ends")

    def __init__(self):
        self.series = [InstantSeries() if i < N_INSTANT else DurationSeries() for i in range(len(METRICS))]
        self.first_q: dict[int, float] = {}
        self.first_q_ends: list[int] = []

    def inst(self, name: str) -> InstantSeries:
        return self.series[M[name]]

    def dur(self, name: str) -> DurationSeries:
        return self.series[M[name]]

    def apply(self, facts, seq: int = 0) -> tuple[int, int]:
        """Apply one accession's facts (metric, rank, start, end, value) as writer ``seq`` (increasing per
        accession); return (restated facts, rank-shadowed facts)."""
        codes = [0, 0, 0]
        for mid, rank, start, end, value in facts:
            if mid < N_INSTANT:
                codes[self.series[mid].put(end, rank, value, seq)] += 1
            else:
                codes[self.series[mid].put(start, end, rank, value, seq)] += 1
        return codes[PUT_RESTATED], codes[PUT_SHADOWED]

    def record_first_quarters(self, ends) -> None:
        """First-reported discrete net-income quarters (SUE input), frozen when first derivable."""
        ni = self.dur("net_income")
        for end in sorted(set(ends)):
            if _within(self.first_q_ends, end, TOL_ANCHOR):
                continue
            q = ni.quarter(end, TOL_SAME_DATE)
            if q is None or not math.isfinite(q[2]) or _within(self.first_q_ends, q[1], TOL_ANCHOR):
                continue
            self.first_q[q[1]] = q[2]
            bisect.insort(self.first_q_ends, q[1])


# ---------------------------------------------------------------------------
# Items at the anchor period (contract section 3.1)
# ---------------------------------------------------------------------------

@dataclass
class AssetSide:
    at: float | None = None
    lt: float | None = None
    che: float | None = None
    debt: float | None = None
    ltd: float | None = None
    noa: float | None = None
    debt_zero_filled: bool = False
    ltd_zero_filled: bool = False
    current_ratio: float | None = None


def _asset_side(st: CikState, target: int, tol: int) -> AssetSide:
    side = AssetSide()
    date = st.inst("total_assets").nearest(target, tol)
    if date is None:
        return side
    g = TOL_SAME_DATE
    at = st.inst("total_assets").get(date, g)
    side.at = at
    lt = st.inst("total_liabilities").get(date, g)
    if lt is None:
        eq = st.inst("equity_incl_minority").get(date, g)
        if eq is None:
            eq = st.inst("stockholders_equity").get(date, g)
        if eq is not None:
            lt = at - eq
    side.lt = lt
    che = st.inst("cash_and_st_investments").get(date, g)
    if che is None:
        cash = st.inst("cash").get(date, g)
        if cash is not None:
            sti = st.inst("st_investments").get(date, g)
            che = cash + (sti if sti is not None else 0.0)
        else:
            che = st.inst("cash_st_inv").get(date, g)
    side.che = che
    std = st.inst("st_debt").get(date, g)
    ltd = st.inst("lt_debt").get(date, g)
    side.debt_zero_filled = std is None or ltd is None
    side.ltd_zero_filled = ltd is None
    side.ltd = ltd if ltd is not None else 0.0
    side.debt = (std if std is not None else 0.0) + side.ltd
    if lt is not None and che is not None:
        side.noa = at - che - lt + side.debt
    ca = st.inst("current_assets").get(date, g)
    cl = st.inst("current_liabilities").get(date, g)
    if ca is not None and cl is not None and cl > 0:
        side.current_ratio = ca / cl
    return side


def _book_equity(st: CikState, target: int, tol: int):
    se_s, nci_s = st.inst("stockholders_equity"), st.inst("equity_incl_minority")
    d1, d2 = se_s.nearest(target, tol), nci_s.nearest(target, tol)
    if d1 is None and d2 is None:
        return None
    if d1 is None or (d2 is not None and abs(d2 - target) < abs(d1 - target)):
        date = d2
    else:
        date = d1
    g = TOL_SAME_DATE
    equity = se_s.get(date, g)
    if equity is None:
        nci_total = nci_s.get(date, g)
        if nci_total is None:
            return None
        minority = st.inst("minority_int_bs").get(date, g)
        equity = nci_total - (minority if minority is not None else 0.0)
    pref = st.inst("pref_stock").get(date, g)
    return equity - (pref if pref is not None else 0.0)


def _ttm_first(st: CikState, names, target: int, tol: int):
    for name in names:
        got = st.dur(name).ttm(target, tol)
        if got is not None:
            return got
    return None


def _gross_profit_ttm(st: CikState, sale, target: int, tol: int):
    gp = st.dur("gross_profit").ttm(target, tol)
    if gp is None and sale is not None:
        cogs = st.dur("cogs").ttm(target, tol)
        if cogs is not None:
            gp = sale - cogs
    return gp


def _shares(st: CikState, anchor: int, counts: dict | None = None):
    """(shrs_q, shrs_q_lag4, rejected) from the first weighted-average share concept present at the anchor.

    Diagnostic only (values unchanged): ``share_pairs_unconfirmed`` counts pairs whose lag value was last written by
    an accession earlier than the one that wrote the current value, i.e. the comparative was not re-reported and
    may predate a split restatement (T25 audits those against known splitters).
    """
    for name in ("shares_diluted_avg", "shares_basic_avg"):
        series = st.dur(name)
        for lo, hi, mid in ((Q_MIN, Q_MAX, 91), (Y_MIN, Y_MAX, 365)):
            cur = series.direct(anchor, TOL_ANCHOR, lo, hi, mid)
            if cur is None:
                continue
            cur_v = cur[2]
            lag = series.direct(anchor - LAG4, TOL_LAG, lo, hi, mid)
            if not (math.isfinite(cur_v) and cur_v > 0):
                return None, None, False
            if lag is not None and not eff.plausible_share_pair(cur_v, lag[2]):
                return None, None, True
            if (lag is not None and counts is not None
                    and series.seq(lag[0], lag[1]) < series.seq(cur[0], cur[1])):
                counts["share_pairs_unconfirmed"] = counts.get("share_pairs_unconfirmed", 0) + 1
            return cur_v, (lag[2] if lag is not None else None), False
    return None, None, False


def _sue(st: CikState, anchor: int):
    lo = bisect.bisect_left(st.first_q_ends, anchor - SUE_WINDOW_DAYS)
    hi = bisect.bisect_right(st.first_q_ends, anchor + TOL_ANCHOR)
    ends = st.first_q_ends[lo:hi]
    if len(ends) < 5 or abs(ends[-1] - anchor) > TOL_ANCHOR:
        return None
    quarters = {date_of(e): (date_of(e), st.first_q[e]) for e in ends}
    got = eff.sue_from_quarters(quarters, finite_statistics=True)
    if got is None or abs(day_of(got[0]) - anchor) > TOL_ANCHOR or not math.isfinite(got[1]):
        return None
    return got[1]


def _fscore(st, anchor, side0, side4, side8, ni0, cfo0, sale0, gp0, sstk0):
    ni4 = st.dur("net_income").ttm(anchor - LAG4, TOL_LAG)
    sale4 = st.dur("revenue").ttm(anchor - LAG4, TOL_LAG)
    gp4 = _gross_profit_ttm(st, sale4, anchor - LAG4, TOL_LAG)
    at0, at4, at8 = side0.at, side4.at, side8.at
    values = (ni0, cfo0, sale0, gp0, sstk0, ni4, sale4, gp4, at0, at4, at8, side0.current_ratio,
              side4.current_ratio)
    if any(v is None or not math.isfinite(v) for v in values):
        return None, False
    if at0 <= 0 or at4 <= 0 or at8 <= 0 or sale0 <= 0 or sale4 <= 0:
        return None, False
    roa0, roa4 = ni0 / at4, ni4 / at8
    lev0 = side0.ltd / ((at0 + at4) / 2.0)
    lev4 = side4.ltd / ((at4 + at8) / 2.0)
    signals = (roa0 > 0, cfo0 > 0, roa0 > roa4, cfo0 > ni0, lev0 <= lev4,
               side0.current_ratio > side4.current_ratio, sstk0 <= 0, gp0 / sale0 > gp4 / sale4,
               sale0 / at4 > sale4 / at8)
    return float(sum(signals)), side0.ltd_zero_filled or side4.ltd_zero_filled


def compute_items(st: CikState, anchor: int, counts: dict) -> tuple[list, str]:
    """All 28 item values (NaN when not derivable) at anchor period ``anchor`` and the zero-fill list."""
    v: dict[str, float | None] = {}
    zero: set[str] = set()
    side0 = _asset_side(st, anchor, TOL_ANCHOR)
    side4 = _asset_side(st, anchor - LAG4, TOL_LAG)
    side8 = _asset_side(st, anchor - LAG8, TOL_LAG)
    v["be"] = _book_equity(st, anchor, TOL_ANCHOR)
    v["at"], v["lt"], v["che"], v["noa"] = side0.at, side0.lt, side0.che, side0.noa
    v["debt"] = side0.debt if side0.at is not None else None
    if side0.at is not None and side0.debt_zero_filled:
        zero.add("debt")
        if side0.noa is not None:
            zero.add("noa")
    v["at_lag4"], v["noa_lag4"] = side4.at, side4.noa
    if side4.noa is not None and side4.debt_zero_filled:
        zero.add("noa_lag4")
    v["be_lag1q"] = _book_equity(st, anchor - LAG1Q, TOL_LAG)
    v["be_lag1q_lag4"] = _book_equity(st, anchor - LAG1Q - LAG4, TOL_LAG)

    sale = st.dur("revenue").ttm(anchor, TOL_ANCHOR)
    v["sale_ttm"] = sale
    v["gp_ttm"] = _gross_profit_ttm(st, sale, anchor, TOL_ANCHOR)
    v["oi_ttm"] = st.dur("operating_income").ttm(anchor, TOL_ANCHOR)
    v["ni_ttm"] = st.dur("net_income").ttm(anchor, TOL_ANCHOR)
    cfo = _ttm_first(st, ("operating_cash_flow", "cfo_continuing"), anchor, TOL_ANCHOR)
    v["cfo_ttm"] = cfo
    v["capx_ttm"] = st.dur("capital_expenditures").ttm(anchor, TOL_ANCHOR)
    v["xrd_ttm"] = st.dur("rd_expense").ttm(anchor, TOL_ANCHOR)
    for item, names in (("dvc_ttm", ("common_div_paid", "dividends_paid")),
                        ("prstkc_ttm", ("share_repurchases",)), ("sstk_ttm", ("stock_issuance",))):
        got = _ttm_first(st, names, anchor, TOL_ANCHOR)
        if got is None and cfo is not None:
            got = 0.0
            zero.add(item)
        v[item] = got
    ni = st.dur("net_income")
    q = ni.quarter(anchor, TOL_ANCHOR)
    v["ni_q"] = None if q is None else q[2]
    q = ni.quarter(anchor - LAG4, TOL_LAG)
    v["ni_q_lag4"] = None if q is None else q[2]
    tax = st.dur("income_tax")
    q = tax.quarter(anchor, TOL_ANCHOR)
    v["txt_q"] = None if q is None else q[2]
    q = tax.quarter(anchor - LAG4, TOL_LAG)
    v["txt_q_lag4"] = None if q is None else q[2]
    v["shrs_q"], v["shrs_q_lag4"], rejected = _shares(st, anchor, counts)
    if rejected:
        counts["share_pairs_rejected"] = counts.get("share_pairs_rejected", 0) + 1
    v["sue"] = _sue(st, anchor)
    fscore, fs_zero = _fscore(st, anchor, side0, side4, side8, v["ni_ttm"], cfo, sale, v["gp_ttm"],
                              v["sstk_ttm"])
    v["fscore"] = fscore
    if fscore is not None and (fs_zero or "sstk_ttm" in zero):
        zero.add("fscore")
    out = []
    for name in ITEMS:
        value = v[name]
        out.append(float(value) if value is not None and math.isfinite(value) else NAN)
    for name in zero:
        counts.setdefault("zero_filled", {}).setdefault(name, 0)
        counts["zero_filled"][name] += 1
    return out, ",".join(sorted(zero))


# ---------------------------------------------------------------------------
# Per-CIK event emission
# ---------------------------------------------------------------------------

@dataclass
class Accession:
    adsh: str
    clock_us: int
    basis: str
    filed: int
    form: str
    sub_period: int | None
    fy: int | None
    fp: str | None
    fye: str | None
    facts: list = field(default_factory=list)  # (metric index, rank, start|None, end, value)


def report_period(acc: Accession, counts: dict | None = None):
    """FSDS ``period`` snapped to the nearest core fact end within 10 d; else the latest core fact end (both bounded
    by the filing date). FSDS rounds ``period`` to a month end, so mid-month fiscal periods take the fallback."""
    core_ends = sorted({f[3] for f in acc.facts if f[0] in CORE_METRICS and f[3] <= acc.filed})
    if not core_ends:
        return None
    if acc.sub_period is not None:
        near = _within(core_ends, acc.sub_period, TOL_SNAP)
        if near:
            return near[0]
        if counts is not None:
            counts["report_period_snap_fallback"] = counts.get("report_period_snap_fallback", 0) + 1
    return core_ends[-1]


@lru_cache(maxsize=1)
def _revenue_ranks() -> tuple[int, frozenset]:
    concept_to = load_concept_map().concept_to
    return concept_to["Revenues"][1], frozenset(concept_to[c][1] for c in RFC_CONCEPTS)


def _count_concept_disagreements(applied: list, counts: dict) -> None:
    """Diagnostics: metric keys tagged by several concepts of one metric with different values in one accession,
    and the revenue keys where an ASC 606 contract-revenue concept is below the `Revenues` total."""
    by_key: dict = {}
    for mid, rank, start, end, value in applied:
        by_key.setdefault((mid, start, end), []).append((rank, value))
    rev_mid = M["revenue"]
    for (mid, _start, _end), entries in by_key.items():
        if len(entries) < 2:
            continue
        best = min(entries)[1]
        if any(not _same_value(v, best) for _r, v in entries):
            per = counts.setdefault("concept_disagreement_keys", {})
            per[METRICS[mid]] = per.get(METRICS[mid], 0) + 1
        if mid == rev_mid:
            total_rank, rfc_ranks = _revenue_ranks()
            total = [v for r, v in entries if r == total_rank]
            if total and any(r in rfc_ranks and v < total[0] and not _same_value(v, total[0]) for r, v in entries):
                counts["revenue_rfc_lt_revenues_keys"] = counts.get("revenue_rfc_lt_revenues_keys", 0) + 1


def process_cik(cik: int, accessions: list[Accession], counts: dict, emit_from_us: int = EMIT_FROM_US) -> list:
    """Replay one CIK's accessions in clock order; emit one snapshot row per accession in the window."""
    rows: list = []
    st = CikState()
    anchor: int | None = None
    anchor_fiscal: tuple[int | None, str | None, str | None] = (None, None, None)
    last_quarterly_us: int | None = None
    ni_mid = M["net_income"]
    for seq, acc in enumerate(sorted(accessions, key=lambda a: (a.clock_us, a.adsh)), start=1):
        if acc.clock_us >= SEAL_US:
            counts["accessions_sealed"] = counts.get("accessions_sealed", 0) + 1
            continue
        facts = sorted(acc.facts, key=lambda f: (f[0], f[1], -1 if f[2] is None else f[2], f[3], f[4]))
        deduped: dict = {}
        for f in facts:
            key = f[:4]
            if key in deduped and not _same_value(deduped[key], f[4]):
                counts["in_accession_conflicts"] = counts.get("in_accession_conflicts", 0) + 1
            deduped[key] = f[4]  # sorted by value: the largest conflicting value wins, deterministically
        applied = [(k[0], k[1], k[2], k[3], val) for k, val in deduped.items()]
        _count_concept_disagreements(applied, counts)
        restated, shadowed = st.apply(applied, seq)
        counts["restated_facts"] = counts.get("restated_facts", 0) + restated
        if shadowed:
            counts["rank_shadowed_facts"] = counts.get("rank_shadowed_facts", 0) + shadowed
        st.record_first_quarters(f[3] for f in applied if f[0] == ni_mid)
        rp = report_period(acc, counts)
        if rp is not None and (anchor is None or rp > anchor):
            anchor = rp
            anchor_fiscal = (acc.fy, acc.fp, acc.fye)
        if acc.form in QUARTERLY_FORMS:
            last_quarterly_us = acc.clock_us
        if acc.clock_us < emit_from_us:
            continue
        if anchor is None:
            counts["rows_without_anchor"] = counts.get("rows_without_anchor", 0) + 1
            continue
        quarterly = (last_quarterly_us is not None
                     and acc.clock_us - last_quarterly_us < ANNUAL_ONLY_LOOKBACK * US_PER_DAY)
        items, zero = compute_items(st, anchor, counts)
        rows.append((cik, acc.adsh, acc.clock_us, acc.basis, acc.filed, acc.form, rp, anchor,
                     anchor_fiscal[0], anchor_fiscal[1], anchor_fiscal[2],
                     STALE_QUARTERLY if quarterly else STALE_ANNUAL_ONLY, *items, zero, len(applied), restated))
        if acc.basis == BASIS_FC1:
            counts["rows_fc1"] = counts.get("rows_fc1", 0) + 1
    return rows


def rows_to_table(rows: list) -> pa.Table:
    rows = sorted(rows, key=lambda r: (r[0], r[2], r[1]))
    cols = list(zip(*rows)) if rows else [[] for _ in EVENT_SCHEMA]
    arrays = []
    for i, fld in enumerate(EVENT_SCHEMA):
        values = list(cols[i])
        if fld.name == "accepted_utc":
            arrays.append(pa.array(values, type=pa.int64()).cast(fld.type))
        elif fld.name in ("filed", "report_period", "period_end"):
            arrays.append(pa.array(values, type=pa.int32()).cast(pa.date32()))
        else:
            arrays.append(pa.array(values, type=fld.type))
    return pa.Table.from_arrays(arrays, schema=EVENT_SCHEMA)


# ---------------------------------------------------------------------------
# Inputs: CIK list, CF-R manifest, FSDS SUB clock table
# ---------------------------------------------------------------------------

def read_cik_list(path: Path) -> list[int]:
    path = Path(path)
    suffix = path.suffix.lower()
    if suffix == ".parquet":
        raw = pq.read_table(path, columns=["cik"]).column("cik").to_pylist()
    elif suffix == ".csv":
        with path.open(newline="", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            if "cik" not in (reader.fieldnames or []):
                raise UsageError(f"{path}: CSV needs a 'cik' column")
            raw = [row["cik"] for row in reader]
    else:
        raw = []
        for line in path.read_text(encoding="utf-8").splitlines():
            token = line.split("#", 1)[0].strip()
            if token:
                raw.append(token)
    ciks = set()
    for value in raw:
        if value is None or (isinstance(value, str) and not value.strip()):
            continue
        try:
            cik = int(str(value).strip())
        except ValueError:
            raise UsageError(f"{path}: invalid CIK {value!r}") from None
        if cik <= 0:
            raise UsageError(f"{path}: invalid CIK {value!r}")
        ciks.add(cik)
    if not ciks:
        raise UsageError(f"{path}: empty CIK list")
    return sorted(ciks)


def load_cf_manifest(cf_dir: Path, expected_sha: str) -> dict:
    raw = (cf_dir / "manifest.json").read_bytes()
    got = hashlib.sha256(raw).hexdigest()
    if got != expected_sha:
        raise UsageError(f"CF-R manifest SHA-256 {got} != pinned {expected_sha}")
    manifest = json.loads(raw)
    batches = manifest.get("batches")
    if not isinstance(batches, list) or not batches:
        raise UsageError("CF-R manifest has no batches")
    out = []
    for b in batches:
        out.append({"batch_id": int(b["batch_id"]), "file": str(b["file"]), "parquet_sha256": str(b["parquet_sha256"]),
                    "first_cik": int(b["first_cik"]), "last_cik": int(b["last_cik"]), "rows": int(b["rows"])})
    ids = [b["batch_id"] for b in out]
    if ids != list(range(len(out))):
        raise UsageError("CF-R manifest batch ids are not 0..n-1 in order")
    return {"manifest_sha256": got, "archive_sha256": str(manifest.get("archive", {}).get("sha256", "")),
            "rule_version": str(manifest.get("rule_version", "")), "batches": out}


def sub_quarters(fsds_dir: Path, expected_sha: str) -> tuple[str, list]:
    raw = (fsds_dir / "fsds-staging-manifest.json").read_bytes()
    got = hashlib.sha256(raw).hexdigest()
    if got != expected_sha:
        raise UsageError(f"FSDS manifest SHA-256 {got} != pinned {expected_sha}")
    quarters = json.loads(raw).get("quarters", {})
    out = []
    for quarter in sorted(quarters):
        if not FIRST_SUB_QUARTER <= quarter <= LAST_SUB_QUARTER:
            continue  # the seal: no SUB quarter after LAST_SUB_QUARTER is opened
        sub = quarters[quarter]["tables"]["sub"]
        out.append({"quarter": quarter, "file": str(sub["path"]).replace("\\", "/"),
                    "sha256": str(sub["parquet_sha256"])})
    if not out or out[-1]["quarter"] != LAST_SUB_QUARTER:
        raise UsageError(f"FSDS manifest lacks SUB quarter {LAST_SUB_QUARTER}")
    return got, out


def _parse_sic(value) -> int | None:
    if value is None:
        return None
    try:
        sic = int(str(value).strip())
    except ValueError:
        return None
    return sic if 100 <= sic <= 9999 else None


def build_clock_tables(fsds_dir: Path, quarters: list, scope: set[int]) -> tuple[pa.Table, pa.Table, dict]:
    """Accession clock table (all filers, SUB <= LAST_SUB_QUARTER) and the in-scope SIC event table."""
    best: dict[str, tuple] = {}
    duplicates = 0
    null_accepted = 0
    for q in quarters:
        path = fsds_dir / q["file"]
        if sha256_file(path) != q["sha256"]:
            raise UsageError(f"FSDS SUB {q['quarter']} SHA-256 does not match its staging manifest")
        t = pq.read_table(path, columns=SUB_COLUMNS)
        adsh = t.column("adsh").to_pylist()
        cik = t.column("cik").to_pylist()
        sic = t.column("sic").to_pylist()
        form = t.column("form").to_pylist()
        period = pc.cast(t.column("period"), pa.int32()).to_pylist()
        fy = t.column("fy").to_pylist()
        fp = t.column("fp").to_pylist()
        filed = pc.cast(t.column("filed"), pa.int32()).to_pylist()
        acc = pc.cast(pc.cast(t.column("accepted_utc"), pa.timestamp("us", tz="UTC")), pa.int64()).to_pylist()
        fye = t.column("fye").to_pylist()
        for i in range(t.num_rows):
            if adsh[i] is None or cik[i] is None or filed[i] is None:
                continue
            if acc[i] is None:
                null_accepted += 1
                clock, basis = filed[i] * US_PER_DAY + FC1_OFFSET_US, BASIS_FC1
            else:
                clock, basis = int(acc[i]), BASIS_FSDS
            row = (adsh[i], int(cik[i]), clock, basis, filed[i], form[i], period[i], fy[i], fp[i], fye[i],
                   _parse_sic(sic[i]))
            prev = best.get(adsh[i])
            if prev is not None:
                duplicates += 1
                if (prev[2], prev[1]) <= (clock, row[1]):
                    continue
            best[adsh[i]] = row
    rows = sorted(best.values())
    cols = list(zip(*rows)) if rows else [[] for _ in CLOCK_SCHEMA]
    clock_table = pa.Table.from_arrays([pa.array(list(c), type=f.type) for c, f in zip(cols, CLOCK_SCHEMA)],
                                       schema=CLOCK_SCHEMA)
    sic_rows = sorted((r[1], r[2], r[0], r[3], r[4], r[5], r[10]) for r in rows
                      if r[1] in scope and r[10] is not None and r[2] < SEAL_US)
    sic_cols = list(zip(*sic_rows)) if sic_rows else [[] for _ in range(7)]
    sic_table = pa.Table.from_arrays(
        [pa.array(list(sic_cols[0]), pa.int64()), pa.array(list(sic_cols[2]), pa.string()),
         pa.array(list(sic_cols[1]), pa.int64()).cast(pa.timestamp("us", tz="UTC")),
         pa.array(list(sic_cols[3]), pa.string()), pa.array(list(sic_cols[4]), pa.int32()).cast(pa.date32()),
         pa.array(list(sic_cols[5]), pa.string()), pa.array(list(sic_cols[6]), pa.int32())],
        schema=SIC_SCHEMA)
    stats = {"sub_rows_unique_accessions": len(rows), "sub_duplicate_accessions": duplicates,
             "sub_null_accepted": null_accepted, "sic_rows": sic_table.num_rows,
             "sic_ciks": len({r[0] for r in sic_rows}), "sic_rows_fc1": sum(1 for r in sic_rows if r[3] == BASIS_FC1)}
    return clock_table, sic_table, stats


# ---------------------------------------------------------------------------
# One CF-R batch
# ---------------------------------------------------------------------------

def read_batch_facts(path: Path, cmap: ConceptMap, scope: set[int], counts: dict) -> pa.Table:
    concepts = sorted(cmap.concept_to)
    filters = [("taxonomy", "=", "us-gaap"), ("filed_date", "<", SEAL), ("form", "in", list(EVENT_FORMS)),
               ("unit", "in", ["USD", "shares"]), ("concept", "in", concepts)]
    t = pq.read_table(path, columns=CF_COLUMNS, filters=filters)
    counts["rows_read"] = counts.get("rows_read", 0) + t.num_rows
    ciks = pc.cast(t.column("cik"), pa.int64())
    t = t.set_column(t.schema.get_field_index("cik"), "cik", ciks)
    t = t.filter(pc.is_in(t.column("cik"), value_set=pa.array(sorted(scope), pa.int64())))
    counts["rows_in_scope"] = counts.get("rows_in_scope", 0) + t.num_rows
    return t.combine_chunks()


def batch_accessions(t: pa.Table, cmap: ConceptMap, clock: pa.Table, counts: dict) -> dict[int, list[Accession]]:
    """Group one batch's in-scope rows into accessions per CIK, attaching the accession clock.

    Row checks are vectorized: unit must match the concept's unit class, value finite, and a duration fact
    needs a start with 1 <= days <= 380 (longer cumulative periods are unused). Instants carry start None.
    """
    if t.num_rows == 0:
        return {}
    concept_list = sorted(cmap.concept_to)
    info = [cmap.concept_to[c] for c in concept_list]
    cidx_arr = pc.index_in(t.column("concept").combine_chunks(), value_set=pa.array(concept_list))
    if cidx_arr.null_count:
        raise UsageError("unmapped concept survived the parquet filter")
    unit_ok = pc.equal(t.column("unit").combine_chunks(), pc.take(pa.array([x[2] for x in info]), cidx_arr))
    value = t.column("value").combine_chunks()
    value_ok = pc.fill_null(pc.is_finite(value), False)
    is_dur = pc.take(pa.array([x[0] >= N_INSTANT for x in info]), cidx_arr)
    start = pc.cast(t.column("period_start").combine_chunks(), pa.int32())
    end = pc.cast(t.column("period_end").combine_chunks(), pa.int32())
    days = pc.add(pc.subtract(end, start), 1)
    shape_ok = pc.or_(pc.invert(is_dur),
                      pc.fill_null(pc.and_(pc.greater_equal(days, 1), pc.less_equal(days, Y_MAX)), False))
    unit_bad = pc.invert(unit_ok)
    value_bad = pc.and_(unit_ok, pc.invert(value_ok))
    shape_bad = pc.and_(pc.and_(unit_ok, value_ok), pc.invert(shape_ok))
    for key, mask in (("unit_mismatch_dropped", unit_bad), ("nonfinite_dropped", value_bad),
                      ("duration_shape_dropped", shape_bad)):
        n = pc.sum(mask).as_py() or 0
        if n:
            counts[key] = counts.get(key, 0) + n
    keep = pc.and_(pc.and_(unit_ok, value_ok), shape_ok)
    t = t.filter(keep)
    if t.num_rows == 0:
        return {}
    cidx = cidx_arr.filter(keep).to_numpy(zero_copy_only=False)
    is_dur_np = is_dur.filter(keep).to_numpy(zero_copy_only=False)
    start_np = pc.fill_null(start.filter(keep), 0).to_numpy(zero_copy_only=False)
    end_np = end.filter(keep).to_numpy(zero_copy_only=False)
    value_np = value.filter(keep).to_numpy(zero_copy_only=False)
    acc_enc = pc.dictionary_encode(t.column("accession_number").combine_chunks())
    acc_idx = acc_enc.indices.to_numpy(zero_copy_only=False)
    acc_names = acc_enc.dictionary.to_pylist()
    ciks_np = _np(t.column("cik"))

    order = np.lexsort((acc_idx, ciks_np))
    cik_o, acc_o = ciks_np[order], acc_idx[order]
    bounds = np.flatnonzero((np.diff(cik_o) != 0) | (np.diff(acc_o) != 0)) + 1
    begins = np.concatenate(([0], bounds)).tolist()
    ends = np.concatenate((bounds, [len(order)])).tolist()
    mids = np.array([x[0] for x in info], dtype=np.int64)[cidx][order].tolist()
    ranks = np.array([x[1] for x in info], dtype=np.int64)[cidx][order].tolist()
    starts = start_np[order].tolist()
    durs = is_dur_np[order].tolist()
    fact_ends = end_np[order].tolist()
    values = value_np[order].astype(np.float64).tolist()
    first = pa.array(order[np.asarray(begins, dtype=np.int64)])
    meta_filed = pc.cast(t.column("filed_date"), pa.int32()).combine_chunks().take(first).to_pylist()
    meta_form = t.column("form").combine_chunks().take(first).to_pylist()
    meta_fy = t.column("fiscal_year").combine_chunks().take(first).to_pylist()
    meta_fp = t.column("fiscal_period").combine_chunks().take(first).to_pylist()
    cik_list = cik_o.tolist()
    acc_list = acc_o.tolist()

    names_needed = pa.array(acc_names)
    sub = clock.filter(pc.is_in(clock.column("adsh"), value_set=names_needed))
    sub_map = {r["adsh"]: r for r in sub.to_pylist()}
    out: dict[int, list[Accession]] = {}
    for g, (b, e) in enumerate(zip(begins, ends)):
        name = acc_names[acc_list[b]]
        s = sub_map.get(name)
        filed = meta_filed[g]
        if s is not None:
            acc = Accession(name, int(s["accepted_us"]), s["clock_basis"], filed, meta_form[g], s["period"],
                            s["fy"], s["fp"], s["fye"])
        else:
            acc = Accession(name, filed * US_PER_DAY + FC1_OFFSET_US, BASIS_FC1, filed, meta_form[g], None,
                            meta_fy[g], meta_fp[g] or None, None)
        acc.facts = [(mids[i], ranks[i], starts[i] if durs[i] else None, fact_ends[i], values[i])
                     for i in range(b, e)]
        out.setdefault(cik_list[b], []).append(acc)
    return out


def run_batch(batch: dict, cf_dir: Path, cmap: ConceptMap, clock: pa.Table, scope: set[int]) -> tuple[pa.Table, dict]:
    counts: dict = {}
    path = cf_dir / batch["file"]
    if sha256_file(path) != batch["parquet_sha256"]:
        raise UsageError(f"CF-R {batch['file']} SHA-256 does not match the CF-R manifest")
    batch_scope = {c for c in scope if batch["first_cik"] <= c <= batch["last_cik"]}
    counts["ciks_in_scope_range"] = len(batch_scope)
    per_cik: dict[int, list[Accession]] = {}
    if batch_scope:  # a batch with no in-scope CIK is verified but never decoded
        t = read_batch_facts(path, cmap, batch_scope, counts)
        per_cik = batch_accessions(t, cmap, clock, counts)
        del t
    rows: list = []
    for cik in sorted(per_cik):
        accs = per_cik[cik]
        counts["accessions"] = counts.get("accessions", 0) + len(accs)
        counts["accessions_fc1"] = counts.get("accessions_fc1", 0) + sum(1 for a in accs if a.basis == BASIS_FC1)
        counts["facts"] = counts.get("facts", 0) + sum(len(a.facts) for a in accs)
        got = process_cik(cik, accs, counts)
        if got:
            counts["ciks_emitted"] = counts.get("ciks_emitted", 0) + 1
        rows.extend(got)
    counts["ciks_with_facts"] = len(per_cik)
    counts["rows"] = len(rows)
    counts["ciks_with_facts_list"] = sorted(per_cik)
    return rows_to_table(rows), counts


# ---------------------------------------------------------------------------
# Stages
# ---------------------------------------------------------------------------

def _run_path(out: Path) -> Path:
    return out / "run.json"


def load_run(out: Path) -> tuple[dict, str]:
    path = _run_path(out)
    if not path.exists():
        raise UsageError(f"{path} missing: run `prepare` first")
    raw = path.read_bytes()
    run = json.loads(raw)
    if run.get("schema") != RUN_SCHEMA:
        raise UsageError(f"{path}: unexpected schema")
    if run["code_sha256"] != code_hashes():
        raise UsageError("code changed since `prepare` (code_sha256 differs); start a fresh --out")
    cmap = load_concept_map()
    if run["concept_map_sha256"] != cmap.sha256:
        raise UsageError("concept map changed since `prepare`")
    return run, hashlib.sha256(raw).hexdigest()


def stage_prepare(args) -> int:
    budget = Budget(args.max_seconds, args.max_rss_mib)
    out = Path(args.out)
    cf_dir, fsds_dir = Path(args.companyfacts_dir), Path(args.fsds_dir)
    cf = load_cf_manifest(cf_dir, args.companyfacts_manifest_sha256)
    fsds_sha, quarters = sub_quarters(fsds_dir, args.fsds_manifest_sha256)
    ciks = read_cik_list(Path(args.cik_list))
    scope_bytes = "".join(f"{c}\n" for c in ciks).encode("ascii")
    cmap = load_concept_map()
    run = {
        "schema": RUN_SCHEMA,
        "tool_version": TOOL_VERSION,
        "companyfacts": {"dir": str(cf_dir.resolve()).replace("\\", "/"), **cf},
        "fsds": {"dir": str(fsds_dir.resolve()).replace("\\", "/"), "manifest_sha256": fsds_sha,
                 "sub_quarters": quarters},
        "cik_list": {"path": str(Path(args.cik_list).resolve()).replace("\\", "/"),
                     "sha256": sha256_file(Path(args.cik_list)), "count": len(ciks),
                     "scope_file": SCOPE_FILE, "scope_sha256": hashlib.sha256(scope_bytes).hexdigest()},
        "parameters": parameters(),
        "code_sha256": code_hashes(),
        "concept_map_sha256": cmap.sha256,
        "concept_map": cmap.by_metric,
    }
    out.mkdir(parents=True, exist_ok=True)
    run_path = _run_path(out)
    if run_path.exists():
        if json.loads(run_path.read_bytes()) != run:
            raise UsageError(f"{run_path} exists with a different plan; use a fresh --out")
        receipt_path = out / "prepare.receipt.json"
        if receipt_path.exists():
            if verify_prepare(out, hashlib.sha256(run_path.read_bytes()).hexdigest()):
                print(canonical({"stage": "prepare", "status": "already_complete"}), flush=True)
                return 0
            raise UsageError(f"{receipt_path} does not verify; delete --out and start again")
    elif any(out.iterdir()):
        raise UsageError(f"{out} is not empty and has no run.json")
    else:
        write_json_atomic(run_path, run)
    run_sha = hashlib.sha256(run_path.read_bytes()).hexdigest()
    scope_tmp = out / ("." + SCOPE_FILE + ".tmp")
    scope_tmp.write_bytes(scope_bytes)
    os.replace(scope_tmp, out / SCOPE_FILE)  # pinned normalized copy: later stages never reread the source
    budget.admit(30.0, "SUB clock table")
    clock, sic, stats = build_clock_tables(fsds_dir, quarters, set(ciks))
    write_parquet_atomic(clock, out / "clock.parquet")
    write_parquet_atomic(sic, out / "sic_events.parquet")
    receipt = {
        "schema": PREPARE_RECEIPT_SCHEMA,
        "run_sha256": run_sha,
        "files": {name: {"sha256": sha256_file(out / name), "bytes": (out / name).stat().st_size, "rows": rows}
                  for name, rows in (("clock.parquet", clock.num_rows), ("sic_events.parquet", sic.num_rows),
                                     (SCOPE_FILE, len(ciks)))},
        "counts": stats,
        "elapsed_s": round(budget.elapsed(), 3),
        "peak_rss_mib": budget.rss() >> 20,
    }
    publish_last(out / "prepare.receipt.json", receipt)
    print(canonical({"stage": "prepare", "status": "complete", **stats,
                     "elapsed_s": receipt["elapsed_s"], "peak_rss_mib": receipt["peak_rss_mib"]}), flush=True)
    return 0


def verify_prepare(out: Path, run_sha: str) -> bool:
    path = out / "prepare.receipt.json"
    if not path.exists():
        return False
    receipt = json.loads(path.read_bytes())
    if receipt.get("schema") != PREPARE_RECEIPT_SCHEMA or receipt.get("run_sha256") != run_sha:
        return False
    return all((out / name).exists() and sha256_file(out / name) == meta["sha256"]
               for name, meta in receipt["files"].items())


def parse_batches(text: str, n: int) -> list[int]:
    try:
        if "-" in text:
            a, b = (int(x) for x in text.split("-", 1))
        else:
            a = b = int(text)
    except ValueError:
        raise UsageError(f"--batches {text!r}: expected N or A-B") from None
    if not 0 <= a <= b < n:
        raise UsageError(f"--batches {text!r} outside 0-{n - 1}")
    return list(range(a, b + 1))


def batch_paths(out: Path, batch_id: int) -> tuple[Path, Path]:
    return out / "events" / f"batch-{batch_id:04d}.parquet", out / "events" / f"batch-{batch_id:04d}.receipt.json"


def verify_batch(out: Path, batch: dict, run_sha: str) -> bool:
    """True when the receipt verifies; False when absent; UsageError when present but not verifying."""
    data, receipt_path = batch_paths(out, batch["batch_id"])
    if not receipt_path.exists():
        return False
    receipt = json.loads(receipt_path.read_bytes())
    ok = (receipt.get("schema") == BATCH_RECEIPT_SCHEMA and receipt.get("run_sha256") == run_sha
          and receipt.get("input", {}).get("sha256") == batch["parquet_sha256"]
          and data.exists() and sha256_file(data) == receipt["output"]["sha256"])
    if not ok:
        raise UsageError(f"{receipt_path} does not verify against run.json and its output; "
                         f"delete batch-{batch['batch_id']:04d}.* and re-run")
    return True


def stage_events(args) -> int:
    budget = Budget(args.max_seconds, args.max_rss_mib)
    out = Path(args.out)
    run, run_sha = load_run(out)
    if not verify_prepare(out, run_sha):
        raise UsageError("prepare receipt missing or not verifying; run `prepare`")
    batches = run["companyfacts"]["batches"]
    wanted = parse_batches(args.batches, len(batches))
    (out / "events").mkdir(exist_ok=True)
    pending = [b for b in (batches[i] for i in wanted) if not verify_batch(out, b, run_sha)]
    skipped = len(wanted) - len(pending)
    if not pending:
        print(canonical({"stage": "events", "status": "complete", "batches": args.batches, "verified_skipped": skipped}),
              flush=True)
        return 0
    cf_dir = Path(run["companyfacts"]["dir"])
    scope_path = out / SCOPE_FILE  # verified by verify_prepare against the prepare receipt
    if sha256_file(scope_path) != run["cik_list"]["scope_sha256"]:
        raise UsageError(f"{scope_path} does not match run.json")
    scope = set(read_cik_list(scope_path))
    cmap = load_concept_map()
    clock = pq.read_table(out / "clock.parquet")
    projected = float(args.projected_batch_seconds)
    done = []
    for batch in pending:
        budget.admit(projected, f"batch {batch['batch_id']}")
        t0 = time.monotonic()
        table, counts = run_batch(batch, cf_dir, cmap, clock, scope)
        data, receipt_path = batch_paths(out, batch["batch_id"])
        write_parquet_atomic(table, data)
        elapsed = time.monotonic() - t0
        projected = max(projected, elapsed * 1.25)
        receipt = {
            "schema": BATCH_RECEIPT_SCHEMA,
            "batch_id": batch["batch_id"],
            "run_sha256": run_sha,
            "input": {"file": batch["file"], "sha256": batch["parquet_sha256"]},
            "output": {"file": f"events/{data.name}", "sha256": sha256_file(data), "bytes": data.stat().st_size,
                       "rows": table.num_rows},
            "counts": counts,
            "elapsed_s": round(elapsed, 3),
            "rss_mib_after": budget.rss() >> 20,
        }
        if receipt_path.exists():
            receipt_path.unlink()  # unreachable after verify_batch; kept for crash-safety symmetry
        publish_last(receipt_path, receipt)
        done.append(batch["batch_id"])
        print(canonical({"stage": "events", "batch": batch["batch_id"], "rows": table.num_rows,
                         "elapsed_s": round(elapsed, 2), "total_s": round(budget.elapsed(), 1),
                         "rss_mib": budget.rss() >> 20}), flush=True)
    print(canonical({"stage": "events", "status": "complete", "batches": args.batches, "computed": done,
                     "verified_skipped": skipped, "elapsed_s": round(budget.elapsed(), 1),
                     "peak_rss_mib": budget.peak >> 20}), flush=True)
    return 0


def parameters() -> dict:
    return {
        "seal": SEAL.isoformat(),
        "emit_from": EMIT_FROM.isoformat(),
        "fc1": "filed 00:00 UTC + 46 h",
        "sub_quarters": f"{FIRST_SUB_QUARTER}..{LAST_SUB_QUARTER}",
        "forms": list(EVENT_FORMS),
        "quarter_days": [Q_MIN, Q_MAX],
        "year_days": [Y_MIN, Y_MAX],
        "tol_anchor_days": TOL_ANCHOR,
        "tol_lag_days": TOL_LAG,
        "tol_same_date_days": TOL_SAME_DATE,
        "tol_snap_days": TOL_SNAP,
        "lags_days": {"lag4": LAG4, "lag1q": LAG1Q, "lag1q_lag4": LAG1Q + LAG4, "lag8": LAG8},
        "sue": "first-reported quarterly net_income; <= 8 previous seasonal differences within "
               f"{SUE_WINDOW_DAYS} d; >= 4 required (export_fundamental_fields.sue_from_quarters)",
        "staleness_days": {"quarterly": STALE_QUARTERLY, "annual_only": STALE_ANNUAL_ONLY,
                           "annual_only_rule": f"no 10-Q/10-QT(/A) accession within {ANNUAL_ONLY_LOOKBACK} d"},
        "zero_fill_items": list(ZERO_FILL_ITEMS),
        "precedence_overrides": {k: list(v) for k, v in PRECEDENCE_OVERRIDES.items()},
        "share_pair_rule": eff.SHARE_PAIR_RULE,
    }


CAVEATS = [
    "values are modeled/unaccepted (not F.1-accepted): bounded v4 producer over CF-R and FSDS SUB",
    "CIK scope is the T19 rehearsal identity bridge (rehearsal_identity=true, scope_complete=false upstream)",
    "CompanyFacts snapshot 2026-09-20 (archive ee099c73...): values as reported in each filing, us-gaap only; "
    "IFRS filers (ifrs-full) and non-USD units are absent",
    "clock = FSDS SUB accepted_utc by accession, else FC1 (filed + 46 h, clock_basis cf_fc1)",
    "canonical metrics follow the atx-db statement map by concept priority, except the total-over-component "
    "precedence overrides (revenue: Revenues before ASC 606 contract revenue; cash: cash and equivalents before "
    "Cash; st_debt: DebtCurrent before its components); seed value_multiplier not applied",
    "a later filing that tags a period under a worse-ranked concept of the same metric does not replace the "
    "better-ranked value (counted as rank_shadowed_facts)",
    "cogs takes one concept by seed priority: with no total tagged, CostOfGoodsSold is used alone (not summed with "
    "CostOfServices), which can overstate gp_ttm for mixed goods/services filers",
    "stock_issuance includes ProceedsFromStockOptionsExercised as a fallback (Compustat SSTK-like), so option "
    "exercise proceeds count as issuance in sstk_ttm and the F-score EQ_OFFER signal",
    "sue uses first-reported quarterly net income on the filing clock (not the earnings-announcement clock)",
    "zero-fill rules: debt components (when total assets exist), dvc/prstkc/sstk (when cfo_ttm exists)",
]


def stage_finalize(args) -> int:
    budget = Budget(args.max_seconds, args.max_rss_mib)
    out = Path(args.out)
    manifest_path = out / "manifest.json"
    if manifest_path.exists():
        raise UsageError(f"{manifest_path} already published")
    run, run_sha = load_run(out)
    if not verify_prepare(out, run_sha):
        raise UsageError("prepare receipt missing or not verifying")
    batches = run["companyfacts"]["batches"]
    missing = [b["batch_id"] for b in batches if not verify_batch(out, b, run_sha)]
    if missing:
        raise UsageError(f"batches without a verified receipt: {missing}")
    tables, receipts = [], []
    for b in batches:
        data, receipt_path = batch_paths(out, b["batch_id"])
        receipts.append(json.loads(receipt_path.read_bytes()))
        tables.append(pq.read_table(data))
    events = pa.concat_tables(tables)
    order = pc.sort_indices(events, sort_keys=[("cik", "ascending"), ("accepted_utc", "ascending"),
                                               ("accession", "ascending")])
    events = events.take(order)
    budget.admit(10.0, "write fundamental_events.parquet")
    write_parquet_atomic(events, out / "fundamental_events.parquet")
    prep = json.loads((out / "prepare.receipt.json").read_bytes())
    counts = summarize(events, receipts, prep, run)
    manifest = {
        "schema": SCHEMA,
        "status": "complete",
        "tool_version": TOOL_VERSION,
        "values_label": "modeled_unaccepted",
        "rehearsal_identity": True,
        "seal": SEAL.isoformat(),
        "emit_from": EMIT_FROM.isoformat(),
        "run_sha256": run_sha,
        "files": {
            "fundamental_events.parquet": {"sha256": sha256_file(out / "fundamental_events.parquet"),
                                           "bytes": (out / "fundamental_events.parquet").stat().st_size,
                                           "rows": events.num_rows},
            "sic_events.parquet": prep["files"]["sic_events.parquet"],
        },
        "items": list(ITEMS),
        "item_units": ITEM_UNITS,
        "inputs": {
            "companyfacts": {k: run["companyfacts"][k] for k in ("dir", "manifest_sha256", "archive_sha256",
                                                                  "rule_version")}
            | {"batches": [{"file": b["file"], "sha256": b["parquet_sha256"]} for b in batches]},
            "fsds": {"dir": run["fsds"]["dir"], "manifest_sha256": run["fsds"]["manifest_sha256"],
                     "sub_quarters": run["fsds"]["sub_quarters"]},
            "cik_list": run["cik_list"],
        },
        "code_sha256": run["code_sha256"],
        "concept_map_sha256": run["concept_map_sha256"],
        "concept_map": run["concept_map"],
        "parameters": run["parameters"],
        "counts": counts,
        "caveats": CAVEATS,
    }
    publish_last(manifest_path, manifest)
    print(canonical({"stage": "finalize", "status": "complete", "rows": events.num_rows,
                     "manifest_sha256": sha256_file(manifest_path), "elapsed_s": round(budget.elapsed(), 1),
                     "peak_rss_mib": budget.rss() >> 20}), flush=True)
    return 0


def _np(column) -> np.ndarray:
    array = column.combine_chunks() if isinstance(column, pa.ChunkedArray) else column
    return array.to_numpy(zero_copy_only=False)


def summarize(events: pa.Table, receipts: list, prep: dict, run: dict) -> dict:
    totals: dict = {}
    zero: dict = {}
    seen: set[int] = set()
    for r in receipts:
        for k, v in r["counts"].items():
            if k == "zero_filled":
                for item, n in v.items():
                    zero[item] = zero.get(item, 0) + n
            elif k == "ciks_with_facts_list":
                seen.update(v)
            elif isinstance(v, int):
                totals[k] = totals.get(k, 0) + v
    # Calendar year of the clock, without a timezone database (UTC microseconds -> datetime64).
    clock_us = _np(events.column("accepted_utc").cast(pa.int64()))
    years = clock_us.astype("datetime64[us]").astype("datetime64[Y]").astype(np.int64) + 1970
    cik_np = _np(events.column("cik"))
    item_np = {name: _np(events.column(name)) for name in ITEMS}
    per_year: dict = {}
    for year in sorted(set(years.tolist())):
        mask = years == year
        per_year[str(year)] = {
            "rows": int(mask.sum()),
            "ciks": int(len(np.unique(cik_np[mask]))),
            "finite_fraction": {name: round(float(np.isfinite(item_np[name][mask]).mean()), 4) for name in ITEMS},
        }
    basis = events.column("clock_basis").to_pylist() if events.num_rows else []
    return {
        "rows": events.num_rows,
        "ciks": len(set(events.column("cik").to_pylist())),
        "ciks_in_scope": run["cik_list"]["count"],
        "ciks_with_facts": len(seen),
        "rows_fc1": sum(1 for b in basis if b == BASIS_FC1),
        "zero_filled": dict(sorted(zero.items())),
        "batch_totals": dict(sorted(totals.items())),
        "prepare": prep["counts"],
        "per_year": per_year,
    }


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="stage", required=True)

    def common(sp, seconds):
        sp.add_argument("--out", required=True, help="output directory (fresh for prepare)")
        sp.add_argument("--max-seconds", type=float, default=seconds, help="cooperative wall-time budget")
        sp.add_argument("--max-rss-mib", type=int, default=1400, help="cooperative RSS budget")

    sp = sub.add_parser("prepare", help="pin inputs, build clock.parquet and sic_events.parquet")
    common(sp, 170.0)
    sp.add_argument("--cik-list", required=True, help="CIK scope (text: one per line; CSV/parquet: 'cik' column)")
    sp.add_argument("--companyfacts-dir", required=True, help="CF-R batch directory")
    sp.add_argument("--companyfacts-manifest-sha256", default=DEFAULT_CF_MANIFEST_SHA256)
    sp.add_argument("--fsds-dir", required=True, help="FSDS v2 staging directory (sub/ + manifest)")
    sp.add_argument("--fsds-manifest-sha256", default=DEFAULT_FSDS_MANIFEST_SHA256)
    sp = sub.add_parser("events", help="build event rows for a CF-R batch range (resumable)")
    common(sp, 170.0)
    sp.add_argument("--batches", required=True, help="batch range A-B (inclusive) or N")
    sp.add_argument("--projected-batch-seconds", type=float, default=8.0,
                    help="initial per-batch time projection for the budget guard")
    sp = sub.add_parser("finalize", help="concatenate verified batches and publish manifest.json last")
    common(sp, 170.0)
    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return {"prepare": stage_prepare, "events": stage_events, "finalize": stage_finalize}[args.stage](args)
    except UsageError as exc:
        print(canonical({"stage": args.stage, "status": "error", "error": str(exc)}), file=sys.stderr, flush=True)
        return 2
    except BudgetStop as exc:
        print(canonical({"stage": args.stage, "status": "budget_stop", "detail": str(exc),
                         "resume": "re-run the same command"}), file=sys.stderr, flush=True)
        return 3


if __name__ == "__main__":
    sys.exit(main())
