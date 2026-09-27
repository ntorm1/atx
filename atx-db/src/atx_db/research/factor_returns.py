"""Factor-return and exposure layer (task P4): risk-model inputs in the research store.

For one sealed R2b feature version (one basis, one R2a panel run) this module builds,
in the research store (RX6, never the warehouse):

* **Market returns**, monthly and daily, restricted to the research universe of the
  basis: equal-weighted over the price-line universe (valid primary lines plus eligible
  lines without an owner link, so the delisted tail the reconstructed bridge cannot link
  stays in: controller ruling on R2b I1) and value-weighted over valid primary lines with
  a **verified** (DEI) market cap only. Unverified vendor/archive caps never weight;
  every exclusion is counted per row. ``mkt_rf`` = VW market minus the risk-free rate.
* **Style factors** SMB / HML / RMW / CMA and UMD, built with the Fama-French 2015 2x3
  *sort mechanics* (NYSE median size x NYSE 30/70 characteristic, value-weighted), from
  R2b **raw** features (``research_feature_matrix.raw_value``, in-domain rows only: e.g.
  negative book is out of the B/M sort) and the R2a verified formation cap. ``smb`` is
  the FF5 average of the size legs of the B/M, OP and INV sorts (NULL unless all three
  sorts form); ``smb_ff3`` is the size leg of the B/M sort alone. **These are not Ken
  French series** (:data:`FACTOR_CONSTRUCTS`, recorded in the run spec and on every style
  row): every sort rebalances *monthly* on the current point-in-time characteristics
  (FF rebalance in June on fiscal-year data); B/M divides the latest book equity by the
  *current* market cap (Asness-Frazzini "HML devil" timing, not FF's December ME); OP is
  TTM operating income over average total assets (Ball et al. 2015), not FF's
  (revenue - COGS - SG&A - interest) / book equity; INV is the latest year-over-year
  total-asset growth (FF: annual).
* **Industry returns**: value-weighted per industry group of every taxonomy the R2b
  context carries (FF12 today: ``classification_basis`` of the version labels how the
  group was assigned). FF49 appears when an R2b version built with a FF49 taxonomy is
  passed in ``industry_versions`` (P6); until then the run carries a blocker.
* **Breakpoints and NYSE-breakpoint universes** (checklist A3): per formation and sort
  variable the 20/30/50/70th percentiles and their venue basis; per formation and
  universe line a ``size_segment`` (micro / small / large by NYSE 20th/50th percentile
  breakpoints) and a cap-rank segment (top 1000 / next 2000 / beyond: rank-style
  segments on verified cap, *not* Russell reconstitution).
* **Per-security exposures** at each formation: market beta, alpha, R^2 and
  idiosyncratic volatility from the trailing 252 sessions of daily excess returns on
  the daily VW market (``beta_mkt_252d`` / ``ivol_252d``: Frazzini-Pedersen 2014 and Ang
  et al. 2006 style inputs, plain OLS), and six-factor betas (``mkt_rf``, SMB, HML, UMD,
  RMW, CMA) with OLS standard errors from the trailing 36 monthly windows (at least 24).
* :func:`span_test`: time-series regression of a long-short series on factor returns
  *over the same windows*; the alpha and every beta are tested with the R3a EWC fixed-b
  robust test (:func:`atx_db.research.stats.mean_inference` on the regression influence
  series, i.e. the EWC sandwich variance with a Student-t(B) reference; the Newey-West t
  is reported beside it). For an overlapping h-month R3b series,
  :func:`span_test_against_run` uses the run's exact h-month rows (below) and tests with
  ``horizon_periods = h``; without them it compounds the monthly rows over h consecutive
  formations (:func:`compound_factor_windows`, labeled ``compounded_21_session_windows``,
  no significance claim at h >= 6: ``significance_claimable``).
* :func:`span_cell` (tier-1 v2 node 4.1): the spanning alpha, its Newey-West t and R^2 of every R3b
  evaluation cell's long-short against each :data:`SPAN_MODELS` model the caller supplies as
  ``BasisInputs.factors`` (atx P4 CAPM / FF5 + UMD analogs via :func:`atx_span_factors`; French FF5 + UMD
  and HXZ q5 benchmark files via :func:`benchmark_span_factors`). Reported, never gating.

Breakpoints and venue (the R3b rule, reused)
--------------------------------------------
The venue is point in time: the strict ``us_listed_v1`` membership row visible at the
formation cutoff (:func:`atx_db.research.evaluation._stage_pit_venue`). Sorts run on the
valid cohort (valid primary lines with a verified cap). Breakpoints are percentiles
(numpy linear interpolation) of the point-in-time NYSE names of that universe when at
least ``nyse_min_names`` of them have the variable (``venue_basis='nyse_pit'``), else of
all names of the universe (``'all_names_fallback'``, controller ruling for R4 policy v2),
else none. Ties: a characteristic equal to a 30/70 breakpoint goes to the lower group
(the rule of :func:`atx_db.research.evaluation.breakpoint_quantiles`); a cap equal to a
size breakpoint (the 2x3 median split and the 20/50 segments) goes to the *upper* group,
as R3b's size buckets do (the NYSE median of an odd reference set is the middle name's
cap). Every factor row, breakpoint, segment and exposure row carries ``basis``,
``venue_basis`` and ``rf_basis``.

Counts on a return row
----------------------
``n_held``: names of the row's universe held for the period (all lines of the formation
universe for market rows, the sort members for a style row, the group for an industry);
``n_names``: those with a return for the period (a valid label; a return that day);
``n_weighted``: those carrying weight (all of ``n_names`` for EW); ``n_excluded_unverified``
and ``n_excluded_no_weight`` partition ``n_names - n_weighted``; ``n_unlabeled`` =
``n_held - n_names``; ``excluded_cap_share`` = unverified (vendor/archive/class-sum) cap
over all known cap of the linked names with a return (measurement only, never a weight).

Return windows and clocks
-------------------------
* **Monthly** rows are the R3a monthly labels at 21 sessions (``forward_returns_
  survivorship_safe``, source :data:`MONTHLY_LABEL_SOURCE`): anchored at the formation's
  entry session, revision-selected and validated by the FQ2 fragment at the observation
  cutoff exactly as R3b reads them, delisting terminals stitched. ``period_date`` is the
  formation date, so a monthly factor row lines up with R3b's ``research_eval_series``
  long-short rows of the same formation (like-for-like spanning tests). Portfolios form
  on every name with a verified cap (and the characteristic) at the formation; returns
  average over names with a valid label, the rest are counted (``n_unlabeled``). Weights
  are the verified formation-date cap (a lagged cap: returns start at the entry close).
* **Exact h-month** rows (``frequency='monthly_h3'/'monthly_h6'/'monthly_h12'``,
  ``FactorSpec.horizons_months``): the same formation portfolios (members, groups and
  weights) over the R3a h-month label window, entry -> entry + 21h sessions (63/126/252
  labels, same revision and FQ2 rules), with ``rf`` over those sessions; only matured
  windows are written. They span an h-month R3b series exactly, where compounding h
  monthly rows gaps or double-counts sessions (a month has 19-23 sessions, a monthly
  label 21) and rebalances monthly instead of holding.
* **Daily** rows are close-to-close returns of the selected bars (the label publisher's
  pick, ``adjusted_close`` as the VA1 ``vendor_artifact_repaired`` series) of the names held
  from the formation close to the last
  session of the next calendar month, capped at the next formation (a missing month never
  extends a hold, holds never overlap and a trade date appears once). A return after a
  halt is attributed to the day of the next trade (CRSP convention; counted as a gap
  return) when its start price lies within ``lookback_calendar_days`` before the
  formation (a per-hold bound, so the result never depends on ``formation_chunk``). A
  delisting terminal return (R3a halt-gap dating,
  ``effective_terminals_sql``) is the name's return on its effective delisting session
  (from the last trade price); prints after it are dropped. **VW weights are the
  verified market cap at the return's start-price session** (the prior session on a
  normal day) from ``market_daily_metrics`` of the basis source, newest row visible by
  that session's 22:00 UTC clock; a cap whose share basis is not DEI is excluded and
  counted, never used.
* ``available_at`` of a factor row is the latest input clock (label, bar, terminal or
  cap clock). Exposures at a formation use only market days and monthly windows that
  ended on or before the formation and whose clocks are at or before its cutoff, so an
  exposure never needs information from after its formation. Label and bar revisions are
  the ones visible at the observation cutoff (realized-outcome vintage, as R3b), filtered
  to rows visible by the formation cutoff.
* The daily market starts at the first formation's hold; ``beta_mkt_252d`` therefore
  needs ``beta_min_sessions`` of it (the first year of formations has none: counted).

Risk-free rate (ruling RX10)
----------------------------
FRED ``DTB3`` (3-month T-bill, secondary market, discount basis, percent per year) from
the graph CSV, fetched once into ``<data dir>/cache/P4-fred-dtb3/DTB3.csv``
(:func:`fetch_fred_dtb3`). The discount rate ``d`` is first converted to the 91-day
bond-equivalent (investment) yield ``y = 365 d / (360 - 91 d)`` (a 5.2% discount rate is
a 5.34% yield; at 15% it is 15.81%); a period starting at session ``s`` then earns
``(1 + y)^(sessions/252) - 1`` with ``d`` the latest observation dated strictly before
``s`` (H.15 publishes a day's rate the next business day at 16:15 ET, before the 22:00
UTC decision clock): ``rf_basis='fred_dtb3_bey_prior_observation'``. Compounding the
simple bond-equivalent yield annually understates the effective annual rate slightly
(5.34% vs 5.45% at d = 5.2%). The FRED current vintage is used, not ALFRED. Without the
cache every row says ``rf_basis='none'``: ``rf`` rows are NULL (status ``rf_basis_none``)
and ``mkt_rf`` equals the raw VW market.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime as dt
import hashlib
import io
import itertools
import json
import math
import re
import sys
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import asdict, dataclass, field, replace
from pathlib import Path
from typing import Any

import duckdb
import numpy as np
import pandas as pd

from .._forward_return_publication import effective_terminals_sql, selected_bars_sql, selected_terminals_sql
from ..connection import resolve_data_dir
from ..market_daily import MARKET_DAILY_SOURCE_NAME, MARKET_DAILY_STRICT_SOURCE_NAME
from ..sec_http import APPROVED_SEC_USER_AGENT
from . import evaluation as _evaluation
from . import stats
from .features import IN_DOMAIN, OWNER_BASIS_LINKED, OWNER_BASIS_UNLINKED
from .labels import (
    HORIZON_FORMATION_UNITS,
    LABEL_CALCULATION_VERSION,
    MONTHLY_LABEL_SOURCE,
    label_revision_order_sql,
    label_status_sql,
)
from .labels import _calendar_keys as _label_calendar_keys  # the label calendar identity (same package)
from .panel import CALENDAR_FORMED, OWNER_LINK_FAILURES, VERIFIED_SHARES_SOURCES
from .store import ResearchStore

# v4 (VA1): daily bar returns read the vendor_artifact_repaired adjusted close (selected_bars_sql)
# and the monthly/h-month rows read forward_return_publication_v3 labels.
FACTOR_VERSION = "research-factor-returns-v4"
FACTOR_SCHEMA_VERSION = 2
BASES = ("strict", "reconstructed")
#: market_daily source of each basis (the R2a panel's own mapping).
MARKET_SOURCES = {"strict": MARKET_DAILY_STRICT_SOURCE_NAME, "reconstructed": MARKET_DAILY_SOURCE_NAME}
NYSE_EXCHANGE_CODE = _evaluation.NYSE_EXCHANGE_CODE
VENUE_NYSE_PIT = "nyse_pit"
VENUE_ALL_NAMES = "all_names_fallback"
VENUE_NONE = "none"
VENUE_NOT_APPLICABLE = "not_applicable"
RF_NONE = "none"
RF_DTB3 = "fred_dtb3_bey_prior_observation"
RF_CONVERSION = ("d = DTB3/100 (discount basis) of the latest observation dated before the period start; "
                 "y = 365 d / (360 - 91 d) (91-day bond-equivalent yield); (1 + y)^(sessions/252) - 1")
FRED_DTB3_URL = "https://fred.stlouisfed.org/graph/fredgraph.csv?id=DTB3"
#: Ruling RX10: the fetch is cached under a P4-prefixed cache directory.
RF_CACHE_DIRNAME = "P4-fred-dtb3"
RF_FILENAME = "DTB3.csv"
USER_AGENT = APPROVED_SEC_USER_AGENT
SESSIONS_PER_YEAR = 252
MONTHLY_LABEL_SESSIONS = 21
FREQ_DAILY, FREQ_MONTHLY = "daily", "monthly"
FAMILY_MARKET, FAMILY_STYLE, FAMILY_INDUSTRY = "market", "style", "industry"
SCOPE_ALL_LINES = "valid_primary_and_unlinked_lines"
SCOPE_VERIFIED = "valid_primary_lines_verified_cap"
WEIGHT_EQUAL = "equal"
WEIGHT_FORMATION_CAP = "value_formation_verified_cap"
WEIGHT_PRIOR_DAY_CAP = "value_prior_session_verified_cap"
STATUS_COMPUTED = "computed"
SIZE_FEATURE = "market_cap"
#: 2x3 sorts: factor -> (R2b characteristic, long group, short group); groups 1..3 are the
#: 30th/70th percentile terciles (1 = low). CMA is long conservative (low asset growth).
SORTS: dict[str, tuple[str, int, int]] = {
    "hml": ("book_to_market", 3, 1),
    "rmw": ("operating_profitability", 3, 1),
    "cma": ("asset_growth", 1, 3),
    "umd": ("momentum_12_1", 3, 1),
}
#: FF5 SMB: the average of the size legs of these sorts.
SMB_SORTS = ("hml", "rmw", "cma")
STYLE_FACTORS = ("smb", "smb_ff3", "hml", "rmw", "cma", "umd")
#: Monthly regressors of the 36-month exposures (in this order).
EXPOSURE_FACTORS = ("mkt_rf", "smb", "hml", "umd", "rmw", "cma")
SORT_PERCENTILES = (30.0, 70.0)
SIZE_PERCENTILE = 50.0
SEGMENT_PERCENTILES = (20.0, 50.0)
SIZE_SEGMENTS = ("micro", "small", "large")
EXPOSURE_ESTIMATED = "estimated"
#: Multi-month horizons with published R3a labels (63/126/252 sessions): exact h-month factor rows.
LABEL_HORIZON_MONTHS = tuple(sorted(units for units in HORIZON_FORMATION_UNITS.values() if units > 1))
#: How a span test's factor windows relate to the series' windows.
WINDOW_EXACT = "exact_label_window"
WINDOW_COMPOUNDED = "compounded_21_session_windows"
WINDOW_CALLER = "caller_supplied"
#: Compounded 21-session windows carry a turn-of-month bias at long horizons (P4 re-review N1):
#: no significance claim is made from them at or beyond this horizon.
COMPOUNDED_CLAIM_MAX_MONTHS = 5
STATUS_RF_NONE = "rf_basis_none"
STATUS_NO_RF = "no_risk_free_observation"
REBALANCE = "monthly"
#: What each style factor is, and how it deviates from the Ken French series (M7).
FACTOR_CONSTRUCTS: dict[str, dict[str, str]] = {
    "hml": {"construct": "2x3 NYSE median size x NYSE 30/70 book_to_market, VW, monthly rebalance",
            "construct_deviation": "B/M = latest common book equity (filing clock) / current market cap at each "
                                   "monthly formation (HML-devil timing); FF: June rebalance, December ME"},
    "rmw": {"construct": "2x3 NYSE median size x NYSE 30/70 operating_profitability, VW, monthly rebalance",
            "construct_deviation": "OP = TTM operating income / average total assets (Ball et al. 2015); "
                                   "FF: (revenue - COGS - SG&A - interest) / book equity, annual"},
    "cma": {"construct": "2x3 NYSE median size x NYSE 30/70 asset_growth (long low growth), VW, monthly rebalance",
            "construct_deviation": "INV = latest year-over-year total-asset growth (quarterly states); FF: annual "
                                   "total-asset growth, June rebalance"},
    "umd": {"construct": "2x3 NYSE median size x NYSE 30/70 momentum_12_1, VW, monthly rebalance",
            "construct_deviation": "momentum = return from 252 to 21 sessions before formation (FF/French UMD: "
                                   "months t-12..t-2); otherwise the French construction"},
    "smb": {"construct": "FF5 SMB: mean of the size legs of the monthly B/M, OP and INV 2x3 sorts",
            "construct_deviation": "inherits the monthly rebalance and characteristic deviations of hml/rmw/cma"},
    "smb_ff3": {"construct": "size leg of the monthly B/M 2x3 sort",
                "construct_deviation": "inherits the hml deviations"},
}

_ID = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_VERSION = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$")
_SLUG = re.compile(r"[^a-z0-9]+")
_NAN = float("nan")
_CODE_FILES = (Path(__file__), Path(stats.__file__), Path(_evaluation.__file__),
               Path(_evaluation.__file__).with_name("labels.py"),
               Path(_evaluation.__file__).parent.parent / "_forward_return_publication.py")


class FactorInputError(ValueError):
    """The inputs cannot produce factor returns under this module's contract."""


class FactorLookaheadError(FactorInputError):
    """An input value is dated after the formation it would be used at."""


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False, default=str)


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _code_sha() -> str:
    digest = hashlib.sha256()
    for path in _CODE_FILES:
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes().replace(b"\r\n", b"\n"))
    return digest.hexdigest()


def _now() -> dt.datetime:
    return dt.datetime.now(dt.UTC).replace(tzinfo=None)


def _naive_utc(value: Any, label: str) -> dt.datetime:
    if not isinstance(value, dt.datetime):
        raise FactorInputError(f"{label} must be a datetime")
    if value.tzinfo is not None:
        if value.utcoffset() != dt.timedelta(0):
            raise FactorInputError(f"{label} must be UTC")
        value = value.astimezone(dt.UTC).replace(tzinfo=None)
    return value


def _float(value: Any) -> float | None:
    if value is None:
        return None
    number = float(value)
    return number if math.isfinite(number) else None


def _stamp(value: Any) -> dt.datetime | None:
    if value is None or (isinstance(value, float) and math.isnan(value)) or pd.isna(value):
        return None
    return pd.Timestamp(value).to_pydatetime()


def _max_stamp(values: np.ndarray, mask: np.ndarray) -> dt.datetime | None:
    chosen = values[mask]
    chosen = chosen[~np.isnat(chosen)]
    return None if not len(chosen) else pd.Timestamp(chosen.max()).to_pydatetime()


def _slug(value: str) -> str:
    return _SLUG.sub("_", value.lower()).strip("_") or "unnamed"


def _bools(frame: pd.DataFrame, column: str) -> np.ndarray:
    return frame[column].fillna(False).to_numpy(dtype=bool)


def _floats(frame: pd.DataFrame, column: str) -> np.ndarray:
    return pd.to_numeric(frame[column], errors="coerce").to_numpy(dtype=float)


def _stamps(frame: pd.DataFrame, column: str) -> np.ndarray:
    return pd.to_datetime(frame[column]).to_numpy(dtype="datetime64[us]")


# ---------------------------------------------------------------------------
# Risk-free rate (ruling RX10)
# ---------------------------------------------------------------------------

@dataclass(frozen=True, eq=False)
class RiskFreeSeries:
    """Daily annualized T-bill observations (percent) and how a period's rate is taken."""

    basis: str
    observation_dates: np.ndarray = field(default_factory=lambda: np.zeros(0, dtype="datetime64[D]"))
    annual_percent: np.ndarray = field(default_factory=lambda: np.zeros(0))
    source: str | None = None
    sha256: str | None = None

    @classmethod
    def none(cls) -> RiskFreeSeries:
        return cls(RF_NONE)

    @classmethod
    def from_observations(cls, rows: Iterable[tuple[Any, Any]], *, basis: str = RF_DTB3, source: str | None = None,
                          sha256: str | None = None) -> RiskFreeSeries:
        pairs = sorted((pd.Timestamp(day).date(), float(rate)) for day, rate in rows
                       if rate is not None and math.isfinite(float(rate)))
        if not pairs:
            raise FactorInputError("a risk-free series needs at least one finite observation")
        if len({day for day, _ in pairs}) != len(pairs):
            raise FactorInputError("duplicate risk-free observation dates")
        return cls(basis, np.array([day for day, _ in pairs], dtype="datetime64[D]"),
                   np.array([rate for _, rate in pairs]), source, sha256)

    def annual_rate_before(self, days: Any) -> np.ndarray:
        """Annual percent of the latest observation dated strictly before each day (NaN if none)."""
        query = np.asarray(days, dtype="datetime64[D]")
        if self.basis == RF_NONE or not len(self.observation_dates):
            return np.full(query.shape, _NAN)
        position = np.searchsorted(self.observation_dates, query, side="left") - 1
        return np.where(position >= 0, self.annual_percent[np.maximum(position, 0)], _NAN)

    def bond_equivalent_yield_before(self, days: Any) -> np.ndarray:
        """91-day bond-equivalent yield ``365 d / (360 - 91 d)`` of the discount rate ``d`` dated
        strictly before each day (decimal; NaN without an observation)."""
        discount = self.annual_rate_before(days) / 100.0
        return 365.0 * discount / (360.0 - 91.0 * discount)

    def period_returns(self, starts: Any, sessions: int | np.ndarray) -> np.ndarray:
        """Return over ``sessions`` sessions of a period starting at each date: ``(1 + y)^(sessions/252)
        - 1`` with ``y`` the bond-equivalent yield (0 without a source, NaN without an observation)."""
        query = np.asarray(starts, dtype="datetime64[D]")
        if self.basis == RF_NONE:
            return np.zeros(query.shape)
        return np.power(1.0 + self.bond_equivalent_yield_before(query),
                        np.asarray(sessions, dtype=float) / SESSIONS_PER_YEAR) - 1.0

    def manifest(self) -> dict[str, Any]:
        first = str(self.observation_dates[0]) if len(self.observation_dates) else None
        last = str(self.observation_dates[-1]) if len(self.observation_dates) else None
        return {"rf_basis": self.basis, "source": self.source, "sha256": self.sha256,
                "observations": len(self.observation_dates), "first": first, "last": last,
                "conversion": RF_CONVERSION if self.basis != RF_NONE else None}


def default_rf_path() -> Path:
    return resolve_data_dir() / "cache" / RF_CACHE_DIRNAME / RF_FILENAME


def parse_fred_series(text: str, series_id: str = "DTB3") -> list[tuple[dt.date, float]]:
    """Rows of a FRED graph CSV (``observation_date``/``DATE`` + series column; ``.`` = missing)."""
    frame = pd.read_csv(io.StringIO(text), dtype=str)
    date_column = next((c for c in ("observation_date", "DATE") if c in frame.columns), None)
    if date_column is None or series_id not in frame.columns:
        raise FactorInputError(f"unexpected FRED CSV columns for {series_id}: {list(frame.columns)}")
    days = pd.to_datetime(frame[date_column], errors="coerce")
    values = pd.to_numeric(frame[series_id].replace(".", pd.NA), errors="coerce")
    rows = [(day.date(), float(value)) for day, value in zip(days, values, strict=True)
            if not pd.isna(day) and not pd.isna(value)]
    if not rows:
        raise FactorInputError(f"FRED CSV for {series_id} has no observations")
    return rows


def fetch_fred_dtb3(cache_dir: Path | str | None = None, *, timeout: int = 60, session: Any = None) -> Path:
    """Fetch FRED DTB3 once into the P4 cache (ruling RX10); returns the CSV path.

    Public read-only fetch of the graph CSV (no API key). The file is validated before it
    replaces the cache; a sidecar ``DTB3.meta.json`` records URL, time, digest and range.
    """
    import requests

    directory = Path(cache_dir) if cache_dir is not None else default_rf_path().parent
    client = session if session is not None else requests
    response = client.get(FRED_DTB3_URL, headers={"User-Agent": USER_AGENT}, timeout=timeout)
    response.raise_for_status()
    content = response.content
    rows = parse_fred_series(content.decode("utf-8"))
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / RF_FILENAME
    partial = path.with_suffix(".partial")
    partial.write_bytes(content)
    partial.replace(path)
    meta = {"url": FRED_DTB3_URL, "fetched_at": _now().isoformat(), "sha256": hashlib.sha256(content).hexdigest(),
            "observations": len(rows), "first": rows[0][0].isoformat(), "last": rows[-1][0].isoformat(),
            "series": "DTB3", "units": "percent per annum, discount basis", "ruling": "RX10"}
    (directory / "DTB3.meta.json").write_text(json.dumps(meta, indent=2, sort_keys=True), encoding="utf-8")
    return path


def load_risk_free(path: Path | str | None = None) -> RiskFreeSeries:
    """The cached DTB3 series; without a cache (and no explicit path) ``rf_basis='none'``."""
    target = Path(path) if path is not None else default_rf_path()
    if not target.is_file():
        if path is not None:
            raise FileNotFoundError(f"risk-free file not found: {target}")
        return RiskFreeSeries.none()
    content = target.read_bytes()
    return RiskFreeSeries.from_observations(parse_fred_series(content.decode("utf-8")), basis=RF_DTB3,
                                            source=f"FRED DTB3 graph CSV ({target.name})",
                                            sha256=hashlib.sha256(content).hexdigest())


# ---------------------------------------------------------------------------
# Spec
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class FactorSpec:
    """Everything that determines a factor run (``run_id`` and ``formation_chunk`` excepted)."""

    run_id: str
    feature_version: str
    #: Observation vintage of labels and bars (UTC).
    label_cutoff: dt.datetime
    label_source: str = MONTHLY_LABEL_SOURCE
    #: Further R2b versions of the same panel run whose context carries another taxonomy (FF49).
    industry_versions: tuple[str, ...] = ()
    nyse_min_names: int = 20
    min_portfolio_names: int = 10
    beta_window_sessions: int = 252
    beta_min_sessions: int = 200
    factor_window_months: int = 36
    factor_min_months: int = 24
    rank_segments: tuple[int, int] = (1000, 3000)
    #: Exact h-month factor rows (``frequency='monthly_h{h}'``) from the R3a h-month labels.
    horizons_months: tuple[int, ...] = LABEL_HORIZON_MONTHS
    daily: bool = True
    exposures: bool = True
    verify_panels: bool = True
    formation_chunk: int = 12
    #: Calendar days before a hold window searched for the first return's start price.
    lookback_calendar_days: int = 45


def _int(value: Any, label: str, low: int, high: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise FactorInputError(f"{label} must be an integer {low}..{high}")
    return value


def validate_factor_spec(spec: FactorSpec) -> FactorSpec:
    if not isinstance(spec.run_id, str) or not _ID.fullmatch(spec.run_id):
        raise FactorInputError("run_id must be a lower-case identifier")
    if not isinstance(spec.feature_version, str) or not _VERSION.fullmatch(spec.feature_version):
        raise FactorInputError("feature_version must be a version identifier")
    cutoff = _naive_utc(spec.label_cutoff, "label_cutoff")
    if not isinstance(spec.label_source, str) or not 0 < len(spec.label_source) <= 200:
        raise FactorInputError("label_source must be a bounded source name")
    industry = tuple(spec.industry_versions)
    if len(set(industry)) != len(industry) or spec.feature_version in industry or any(
            not isinstance(v, str) or not _VERSION.fullmatch(v) for v in industry):
        raise FactorInputError("industry_versions must be distinct other version identifiers")
    _int(spec.nyse_min_names, "nyse_min_names", 1, 100_000)
    _int(spec.min_portfolio_names, "min_portfolio_names", 1, 100_000)
    _int(spec.beta_window_sessions, "beta_window_sessions", 20, 2520)
    _int(spec.beta_min_sessions, "beta_min_sessions", 10, spec.beta_window_sessions)
    _int(spec.factor_window_months, "factor_window_months", 6, 120)
    _int(spec.factor_min_months, "factor_min_months", len(EXPOSURE_FACTORS) + 3, spec.factor_window_months)
    _int(spec.formation_chunk, "formation_chunk", 1, 120)
    _int(spec.lookback_calendar_days, "lookback_calendar_days", 7, 400)
    top, total = spec.rank_segments
    _int(top, "rank_segments[0]", 1, 1_000_000)
    _int(total, "rank_segments[1]", top + 1, 1_000_000)
    for name in ("daily", "exposures", "verify_panels"):
        if not isinstance(getattr(spec, name), bool):
            raise FactorInputError(f"{name} must be a bool")
    horizons = tuple(spec.horizons_months)
    if len(set(horizons)) != len(horizons) or any(h not in LABEL_HORIZON_MONTHS for h in horizons):
        raise FactorInputError(f"horizons_months must be distinct values from {LABEL_HORIZON_MONTHS}")
    return replace(spec, label_cutoff=cutoff, industry_versions=industry, horizons_months=tuple(sorted(horizons)))


def spec_payload(spec: FactorSpec) -> dict[str, Any]:
    payload = {k: v for k, v in asdict(spec).items() if k not in ("run_id", "formation_chunk")}
    payload["label_cutoff"] = spec.label_cutoff.isoformat()
    payload["industry_versions"] = list(spec.industry_versions)
    payload["rank_segments"] = list(spec.rank_segments)
    payload["horizons_months"] = list(spec.horizons_months)
    payload.update(horizon_rule=("monthly_h{h}: the formation's monthly portfolios (same members and weights) "
                                 "over the R3a label window entry -> entry + 21h sessions; matured windows only"),
                   factor_version=FACTOR_VERSION, label_calculation_version=LABEL_CALCULATION_VERSION,
                   label_sessions=MONTHLY_LABEL_SESSIONS, sorts={k: list(v) for k, v in SORTS.items()},
                   smb_sorts=list(SMB_SORTS), exposure_factors=list(EXPOSURE_FACTORS),
                   sort_percentiles=list(SORT_PERCENTILES), size_percentile=SIZE_PERCENTILE,
                   segment_percentiles=list(SEGMENT_PERCENTILES),
                   verified_shares_sources=list(VERIFIED_SHARES_SOURCES),
                   breakpoint_rule=("numpy linear percentile; characteristic <= breakpoint -> lower group; "
                                    "cap == size breakpoint -> upper group (R3b size buckets)"),
                   rebalance=REBALANCE, factor_constructs=FACTOR_CONSTRUCTS, rf_conversion=RF_CONVERSION,
                   hold_rule="formation close to the last session of the next month, capped at the next formation")
    return payload


# ---------------------------------------------------------------------------
# Schema (research store; owned here until registered as a store migration)
# ---------------------------------------------------------------------------

_RUN_COLUMNS = (
    ("run_id", "VARCHAR PRIMARY KEY"), ("status", "VARCHAR NOT NULL"), ("basis", "VARCHAR NOT NULL"),
    ("feature_version", "VARCHAR NOT NULL"), ("panel_run_id", "VARCHAR NOT NULL"),
    ("panel_sha256", "VARCHAR NOT NULL"), ("factor_version", "VARCHAR NOT NULL"), ("rf_basis", "VARCHAR NOT NULL"),
    ("rf_sha256", "VARCHAR"), ("label_source", "VARCHAR NOT NULL"), ("label_cutoff", "TIMESTAMP NOT NULL"),
    ("spec_json", "VARCHAR NOT NULL"), ("spec_sha256", "VARCHAR NOT NULL"), ("code_sha256", "VARCHAR NOT NULL"),
    ("blockers_json", "VARCHAR NOT NULL"), ("diagnostic_json", "VARCHAR"), ("results_sha256", "VARCHAR"),
    ("created_at", "TIMESTAMP NOT NULL"), ("finished_at", "TIMESTAMP"))
_RETURN_COLUMNS = (
    ("run_id", "VARCHAR"), ("frequency", "VARCHAR"), ("period_date", "DATE"), ("window_start", "DATE"),
    ("window_end", "DATE"), ("factor_family", "VARCHAR"), ("factor_id", "VARCHAR"), ("value", "DOUBLE"),
    ("n_names", "BIGINT"), ("n_weighted", "BIGINT"), ("n_excluded_unverified", "BIGINT"),
    ("n_excluded_no_weight", "BIGINT"), ("n_unlabeled", "BIGINT"), ("available_at", "TIMESTAMP"),
    ("basis", "VARCHAR"), ("venue_basis", "VARCHAR"), ("rf_basis", "VARCHAR"), ("weighting", "VARCHAR"),
    ("universe_scope", "VARCHAR"), ("status", "VARCHAR"), ("detail_json", "VARCHAR"),
    ("n_held", "BIGINT"), ("excluded_cap_share", "DOUBLE"))
#: Schema v1 -> v2 (P4 fix 1, M5): columns added in place to a v1 store.
_V2_COLUMNS = (("research_factor_returns", "n_held", "BIGINT"),
               ("research_factor_returns", "excluded_cap_share", "DOUBLE"))
_BREAKPOINT_COLUMNS = (
    ("run_id", "VARCHAR"), ("formation_date", "DATE"), ("variable", "VARCHAR"), ("venue_basis", "VARCHAR"),
    ("reference_names", "BIGINT"), ("universe_names", "BIGINT"), ("p20", "DOUBLE"), ("p30", "DOUBLE"),
    ("p50", "DOUBLE"), ("p70", "DOUBLE"), ("basis", "VARCHAR"), ("rf_basis", "VARCHAR"))
_SEGMENT_COLUMNS = (
    ("run_id", "VARCHAR"), ("formation_date", "DATE"), ("security_id", "VARCHAR"), ("owner_basis", "VARCHAR"),
    ("market_cap", "DOUBLE"), ("size_segment", "VARCHAR"), ("cap_rank", "BIGINT"), ("rank_segment", "VARCHAR"),
    ("basis", "VARCHAR"), ("venue_basis", "VARCHAR"), ("rf_basis", "VARCHAR"))
_BETA_COLUMNS = tuple(column for name in EXPOSURE_FACTORS
                      for column in ((f"beta_{name}_36m", "DOUBLE"), (f"se_{name}_36m", "DOUBLE")))
_EXPOSURE_COLUMNS = (
    ("run_id", "VARCHAR"), ("formation_date", "DATE"), ("security_id", "VARCHAR"), ("owner_basis", "VARCHAR"),
    ("beta_mkt_252d", "DOUBLE"), ("se_beta_mkt_252d", "DOUBLE"), ("alpha_252d", "DOUBLE"),
    ("ivol_252d", "DOUBLE"), ("r2_252d", "DOUBLE"), ("n_daily_obs", "BIGINT"), ("status_252d", "VARCHAR"),
    ("alpha_36m", "DOUBLE"), *_BETA_COLUMNS, ("resid_vol_36m", "DOUBLE"), ("r2_36m", "DOUBLE"),
    ("n_monthly_obs", "BIGINT"), ("status_36m", "VARCHAR"), ("available_at", "TIMESTAMP"),
    ("basis", "VARCHAR"), ("venue_basis", "VARCHAR"), ("rf_basis", "VARCHAR"))
_TABLES = {"runs": ("research_factor_runs", _RUN_COLUMNS),
           "returns": ("research_factor_returns", _RETURN_COLUMNS),
           "breakpoints": ("research_factor_breakpoints", _BREAKPOINT_COLUMNS),
           "segments": ("research_factor_segments", _SEGMENT_COLUMNS),
           "exposures": ("research_factor_exposures", _EXPOSURE_COLUMNS)}
#: Deterministic row order of each result table (the results digest streams in this order).
_ORDER = {"returns": "frequency, period_date, factor_id", "breakpoints": "formation_date, variable",
          "segments": "formation_date, security_id", "exposures": "formation_date, security_id"}


def ensure_factor_schema(con: duckdb.DuckDBPyConnection) -> None:
    """Create or migrate the ``research_factor_*`` tables (schema v2; a v1 store gains the v2
    columns in place). Result tables have no primary key (an index over every exposure row
    is not worth its memory); runs replace by run_id. Inserts name their columns."""
    con.execute("CREATE TABLE IF NOT EXISTS research_factor_schema (version INTEGER PRIMARY KEY, "
                "name VARCHAR NOT NULL, applied_at TIMESTAMP NOT NULL)")
    versions = {int(row[0]) for row in con.execute("SELECT version FROM research_factor_schema").fetchall()}
    if versions - {1, FACTOR_SCHEMA_VERSION}:
        raise RuntimeError(f"research factor schema has unknown versions {sorted(versions)}; code is older")
    if FACTOR_SCHEMA_VERSION in versions:
        return
    for table, columns in _TABLES.values():
        con.execute(f"CREATE TABLE IF NOT EXISTS {table} ({', '.join(f'{c} {t}' for c, t in columns)})")
    for table, column, kind in _V2_COLUMNS:  # a no-op on a fresh store
        con.execute(f"ALTER TABLE {table} ADD COLUMN IF NOT EXISTS {column} {kind}")
    con.execute("INSERT INTO research_factor_schema VALUES (?, ?, ?)",
                [FACTOR_SCHEMA_VERSION, "factor_returns_v2_held_and_cap_share", _now()])


_NUMERIC = ("DOUBLE", "BIGINT", "INTEGER")


def _normalized(frame: pd.DataFrame, columns: Sequence[tuple[str, str]]) -> pd.DataFrame:
    """One pandas dtype per declared SQL type (missing columns are NULL)."""
    out: dict[str, pd.Series] = {}
    for name, kind in columns:
        base = kind.split()[0]
        column = frame[name] if name in frame.columns else pd.Series([None] * len(frame), index=frame.index,
                                                                      dtype=object)
        if base in _NUMERIC:
            out[name] = pd.to_numeric(column, errors="coerce").astype(float)
        elif base in ("TIMESTAMP", "DATE"):
            out[name] = pd.to_datetime(column, errors="coerce")
        else:
            values = column.astype(object)
            out[name] = values.where(values.notna(), None)
    return pd.DataFrame(out, index=frame.index)


def _select(columns: Sequence[tuple[str, str]]) -> str:
    """Typed SELECT list; a pandas NaN becomes SQL NULL (never a stored NaN)."""
    parts = []
    for name, kind in columns:
        base = kind.split()[0]
        if base in _NUMERIC:
            parts.append(f"CASE WHEN isnan({name}) THEN NULL ELSE CAST({name} AS {base}) END")
        else:
            parts.append(f"CAST({name} AS {base})")
    return ", ".join(parts)


def _stage_frame(con: duckdb.DuckDBPyConnection, name: str, frame: pd.DataFrame,
                 columns: Sequence[tuple[str, str]], *, into: str | None = None) -> int:
    """Insert ``frame`` into temp table ``name`` (created) or into the existing table ``into``."""
    if into is None:
        con.execute(f"CREATE OR REPLACE TEMP TABLE {name} ({', '.join(f'{c} {t.split()[0]}' for c, t in columns)})")
    if not len(frame):
        return 0
    con.register("_p4_frame", _normalized(frame, columns))
    try:
        con.execute(f"INSERT INTO {into or name} ({', '.join(c for c, _ in columns)}) "
                    f"SELECT {_select(columns)} FROM _p4_frame")
    finally:
        con.unregister("_p4_frame")
    return len(frame)


def _insert(con: duckdb.DuckDBPyConnection, key: str, rows: Sequence[Mapping[str, Any]] | pd.DataFrame) -> int:
    table, columns = _TABLES[key]
    frame = rows if isinstance(rows, pd.DataFrame) else pd.DataFrame(list(rows))
    return _stage_frame(con, "_p4_unused", frame, columns, into=table)


# ---------------------------------------------------------------------------
# Pure pieces: breakpoints, 2x3 sorts, grouped OLS, span test
# ---------------------------------------------------------------------------

def reference_names(values: np.ndarray, universe: np.ndarray, nyse: np.ndarray,
                    min_names: int) -> tuple[np.ndarray | None, str]:
    """The breakpoint reference set: point-in-time NYSE names of the universe with a value,
    else every universe name with a value (labeled fallback), else none."""
    has = universe & np.isfinite(values)
    reference = has & nyse
    if int(reference.sum()) >= min_names:
        return reference, VENUE_NYSE_PIT
    if int(has.sum()) >= min_names:
        return has, VENUE_ALL_NAMES
    return None, VENUE_NONE


def assign_groups(values: np.ndarray, breaks: np.ndarray, member: np.ndarray, *, ties: str = "lower"
                  ) -> np.ndarray:
    """Group 1..len(breaks)+1; 0 outside. A value equal to a breakpoint goes to the lower
    group (``ties='lower'``: characteristic sorts, R3b ``breakpoint_quantiles``) or to the
    upper group (``ties='upper'``: size, R3b ``_size_buckets``)."""
    if ties not in ("lower", "upper"):
        raise ValueError("ties must be 'lower' or 'upper'")
    side = "left" if ties == "lower" else "right"
    groups = np.searchsorted(np.asarray(breaks, dtype=float), np.nan_to_num(values), side=side) + 1
    return np.where(member & np.isfinite(values), groups, 0).astype(np.int8)


def _combine_venues(*venues: str) -> str:
    if any(v == VENUE_NONE for v in venues):
        return VENUE_NONE
    return VENUE_NYSE_PIT if all(v == VENUE_NYSE_PIT for v in venues) else VENUE_ALL_NAMES


def _vw(returns: np.ndarray, weights: np.ndarray, mask: np.ndarray) -> float:
    total = float(weights[mask].sum())
    return float(np.dot(weights[mask], returns[mask]) / total) if mask.any() and total > 0 else _NAN


def grouped_ols(group: np.ndarray, y: np.ndarray, x: np.ndarray, n_groups: int, min_obs: int
                ) -> dict[str, np.ndarray]:
    """Per-group OLS of ``y`` on ``x`` (which includes the intercept column): coefficients,
    classical standard errors, residual sd, R^2 and n; NaN where a group has fewer than
    ``min_obs`` rows or a rank-deficient design."""
    k = x.shape[1]
    group = np.asarray(group, dtype=np.int64)
    n = np.bincount(group, minlength=n_groups)[:n_groups].astype(np.int64)
    xtx = np.zeros((n_groups, k, k))
    xty = np.zeros((n_groups, k))
    for a in range(k):
        xty[:, a] = np.bincount(group, weights=x[:, a] * y, minlength=n_groups)[:n_groups]
        for b in range(a, k):
            xtx[:, a, b] = xtx[:, b, a] = np.bincount(group, weights=x[:, a] * x[:, b], minlength=n_groups)[:n_groups]
    beta = np.full((n_groups, k), _NAN)
    se = np.full((n_groups, k), _NAN)
    resid_sd = np.full(n_groups, _NAN)
    r2 = np.full(n_groups, _NAN)
    candidates = np.flatnonzero(n >= max(min_obs, k + 1))
    status = np.where(n >= max(min_obs, k + 1), EXPOSURE_ESTIMATED, "insufficient_obs").astype(object)
    if len(candidates):
        full = np.linalg.matrix_rank(xtx[candidates]) == k
        status[candidates[~full]] = "rank_deficient"
        ok = candidates[full]
        if len(ok):
            inverse = np.linalg.inv(xtx[ok])
            beta[ok] = np.einsum("gij,gj->gi", inverse, xty[ok])
            fitted = np.einsum("rk,rk->r", x, np.nan_to_num(beta[group]))
            resid = y - fitted
            rss = np.bincount(group, weights=resid * resid, minlength=n_groups)[:n_groups]
            ysum = np.bincount(group, weights=y, minlength=n_groups)[:n_groups]
            ysq = np.bincount(group, weights=y * y, minlength=n_groups)[:n_groups]
            dof = (n[ok] - k).astype(float)
            sigma2 = rss[ok] / dof
            se[ok] = np.sqrt(sigma2[:, None] * np.diagonal(inverse, axis1=1, axis2=2))
            resid_sd[ok] = np.sqrt(sigma2)
            tss = ysq[ok] - ysum[ok] ** 2 / n[ok]
            with np.errstate(divide="ignore", invalid="ignore"):
                r2[ok] = np.where(tss > 0, 1.0 - rss[ok] / tss, _NAN)
    return {"beta": beta, "se": se, "resid_sd": resid_sd, "r2": r2, "n": n, "status": status}


@dataclass(frozen=True)
class SpanTestResult:
    """Time-series spanning regression ``y_t = alpha + b'F_t + e_t`` with EWC robust inference."""

    factors: tuple[str, ...]
    n_obs: int
    span: int
    horizon_periods: int
    alpha: float
    alpha_inference: stats.MeanInference
    betas: dict[str, float]
    beta_inference: dict[str, stats.MeanInference]
    r2: float
    residual_sd: float
    #: How the factor windows were built (:data:`WINDOW_EXACT`, :data:`WINDOW_COMPOUNDED`,
    #: :data:`WINDOW_CALLER`).
    factor_window_basis: str = WINDOW_CALLER
    #: False for compounded 21-session windows at ``horizon_periods >= 6``: the windows then
    #: miss the label window by up to ~h sessions, so no significance claim may be made (P5
    #: must report such a result as descriptive only).
    significance_claimable: bool = True

    @property
    def alpha_robust_p(self) -> float:
        return self.alpha_inference.robust_p_value

    @property
    def alpha_z(self) -> float:
        return self.alpha_inference.z_equivalent

    def as_dict(self) -> dict[str, Any]:
        return {"factors": list(self.factors), "n_obs": self.n_obs, "span": self.span,
                "horizon_periods": self.horizon_periods, "alpha": self.alpha,
                "alpha_inference": self.alpha_inference.as_dict(), "betas": dict(self.betas),
                "beta_inference": {k: v.as_dict() for k, v in self.beta_inference.items()},
                "r2": self.r2, "residual_sd": self.residual_sd,
                "factor_window_basis": self.factor_window_basis,
                "significance_claimable": self.significance_claimable}


def _claimable(factor_window_basis: str, horizon_periods: int) -> bool:
    return not (factor_window_basis == WINDOW_COMPOUNDED and horizon_periods > COMPOUNDED_CLAIM_MAX_MONTHS)


def span_test(ls_returns: pd.Series | Sequence[float], factors: pd.DataFrame | Mapping[str, Sequence[float]], *,
              horizon_periods: int = 1, factor_window_basis: str = WINDOW_CALLER) -> SpanTestResult:
    """Alpha of a long-short series beyond known factors, EWC fixed-b robust (R3a).

    **The factors must cover the same window as each series value**: an overlapping
    h-month series needs h-month compounded factors (:func:`compound_factor_windows`;
    :func:`span_test_against_run` does this), else beta captures about 1/h of the
    co-movement and alpha absorbs the rest of the premia. ``ls_returns`` and ``factors``
    are aligned on the series index (a date or period index is sorted first; positions
    otherwise). The calendar is the series' index: a period missing from the factors keeps
    its position as a gap in every HAC sum, but a period *absent from the series index* is
    not a gap, so pass a complete calendar with NaN rows (the run helper reindexes onto the
    monthly calendar). Each coefficient is tested by :func:`stats.mean_inference` on its
    OLS influence series ``b_j + [(X'X/n)^-1 x_t e_t]_j`` (mean ``b_j``, long-run variance
    = the sandwich), so ``robust_p_value`` / ``z_equivalent`` / ``hlz_pass`` are the EWC
    Student-t(B) test and ``nw_*`` the Newey-West comparison, with ``horizon_periods = h``
    for overlapping h-month series. ``factor_window_basis`` labels the factor windows;
    :data:`WINDOW_COMPOUNDED` at ``horizon_periods >= 6`` sets ``significance_claimable``
    False.
    """
    if factor_window_basis not in (WINDOW_EXACT, WINDOW_COMPOUNDED, WINDOW_CALLER):
        raise FactorInputError(f"unknown factor_window_basis {factor_window_basis!r}")
    y = ls_returns if isinstance(ls_returns, pd.Series) else pd.Series(list(ls_returns), dtype=float)
    frame = factors if isinstance(factors, pd.DataFrame) else pd.DataFrame(dict(factors))
    if not len(frame.columns):
        raise FactorInputError("span_test needs at least one factor")
    if isinstance(y.index, (pd.DatetimeIndex, pd.PeriodIndex)):
        if y.index.has_duplicates:
            raise FactorInputError("span_test: duplicate periods in the long-short series")
        y = y.sort_index()
    if isinstance(ls_returns, pd.Series) and isinstance(factors, pd.DataFrame):
        if frame.index.has_duplicates:
            raise FactorInputError("span_test: duplicate periods in the factors")
        frame = frame.reindex(y.index)
    elif len(frame) != len(y):
        raise FactorInputError("unindexed ls_returns and factors must have equal length")
    else:
        frame.index = y.index
    names = tuple(str(c) for c in frame.columns)
    values = pd.to_numeric(y, errors="coerce").to_numpy(dtype=float)
    regressors = frame.apply(pd.to_numeric, errors="coerce").to_numpy(dtype=float)
    complete = np.isfinite(values) & np.isfinite(regressors).all(axis=1)
    n = int(complete.sum())
    k = len(names) + 1
    if n < k + 2:
        raise FactorInputError(f"span_test needs at least {k + 2} complete periods, got {n}")
    design = np.column_stack([np.ones(n), regressors[complete]])
    target = values[complete]
    q = design.T @ design / n
    if np.linalg.matrix_rank(q) < k:
        raise FactorInputError("span_test design is rank deficient (collinear factors)")
    q_inverse = np.linalg.inv(q)
    coefficients = q_inverse @ (design.T @ target) / n
    residual = target - design @ coefficients
    influence = (design * residual[:, None]) @ q_inverse.T
    inferences = []
    for j in range(k):
        positioned = np.full(len(values), _NAN)
        positioned[complete] = coefficients[j] + influence[:, j]
        inferences.append(stats.mean_inference(positioned, horizon_periods=horizon_periods))
    tss = float(((target - target.mean()) ** 2).sum())
    rss = float((residual ** 2).sum())
    return SpanTestResult(names, n, inferences[0].span, int(horizon_periods), float(coefficients[0]), inferences[0],
                          {name: float(coefficients[j + 1]) for j, name in enumerate(names)},
                          {name: inferences[j + 1] for j, name in enumerate(names)},
                          1.0 - rss / tss if tss > 0 else _NAN, math.sqrt(rss / max(n - k, 1)),
                          factor_window_basis, _claimable(factor_window_basis, int(horizon_periods)))


# ---------------------------------------------------------------------------
# Run staging
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class _Formation:
    month_index: int
    formation_date: dt.date
    cutoff: dt.datetime
    entry_date: dt.date
    session: int | None           # session number of the formation date in the label calendar
    entry_aligned: bool
    label_end: dt.date | None     # entry + 21 sessions
    label_matured: bool
    hold_end: dt.date | None      # last session of the next calendar month


@dataclass
class _Work:
    basis: str
    formations: list[_Formation]
    sessions: list[dt.date]
    session_number: dict[dt.date, int]
    taxonomies: list[tuple[str, str, str]]     # (feature_version, taxonomy, classification_basis)
    characteristics: tuple[str, ...]           # sort characteristics present in the version
    market_source: str
    has_market_daily: bool
    pit_membership: bool
    diag: dict[str, Any]


def _taxonomy(classification_basis: str | None) -> str:
    if classification_basis and ":" in classification_basis:
        return classification_basis.split(":", 1)[1]
    return "unspecified_taxonomy"


def _stage(store: ResearchStore, spec: FactorSpec, table: _evaluation.FeatureTable,
           industry_tables: Sequence[_evaluation.FeatureTable]) -> _Work:
    con = store.con
    calendar = con.execute("""
        SELECT month_start, formation_date, cutoff, entry_date, status
        FROM research_panel_calendar WHERE run_id=? ORDER BY month_start
    """, [table.panel_run_id]).df()
    calendar.insert(0, "month_index", np.arange(len(calendar), dtype=np.int64))
    formed = calendar[calendar["status"] == CALENDAR_FORMED]
    # The relations R3b's point-in-time venue rule reads (same names and shapes as evaluation).
    _stage_frame(con, "_ev_months", pd.DataFrame({
        "formation_date": formed["formation_date"], "month_index": formed["month_index"],
        "cutoff": formed["cutoff"]}), (("formation_date", "DATE"), ("month_index", "BIGINT"), ("cutoff", "TIMESTAMP")))
    securities = [row[0] for row in con.execute(
        "SELECT DISTINCT security_id FROM research_panel_cohort WHERE run_id=? ORDER BY security_id",
        [table.panel_run_id]).fetchall()]
    _stage_frame(con, "_ev_securities", pd.DataFrame({
        "security_id": pd.Series(securities, dtype=object), "code": np.arange(len(securities), dtype=np.int64)}),
        (("security_id", "VARCHAR"), ("code", "BIGINT")))
    pit_membership = _evaluation._stage_pit_venue(con)
    calendar_id, calendar_source = _label_calendar_keys()
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _p4_sessions AS
        SELECT trade_date, row_number() OVER (ORDER BY trade_date) AS session_number
        FROM (SELECT DISTINCT trade_date FROM trading_calendar
              WHERE calendar_id=? AND source=? AND is_open AND trade_date <= CAST(? AS DATE))
    """, [calendar_id, calendar_source, spec.label_cutoff])
    sessions = [row[0] for row in con.execute("SELECT trade_date FROM _p4_sessions ORDER BY session_number").fetchall()]
    number = {day: i + 1 for i, day in enumerate(sessions)}
    month_last: dict[tuple[int, int], dt.date] = {}
    for day in sessions:
        month_last[(day.year, day.month)] = day
    formations = []
    formed_days = [pd.Timestamp(value).date() for value in formed["formation_date"]]
    if any(later <= earlier for earlier, later in itertools.pairwise(formed_days)):
        raise FactorInputError("panel formations are not strictly increasing in date")
    next_formed = dict(zip(formed_days, [*formed_days[1:], None], strict=True))
    for row in formed.itertuples(index=False):
        day = pd.Timestamp(row.formation_date).date()
        entry = pd.Timestamp(row.entry_date).date()
        session = number.get(day)
        aligned = session is not None and session < len(sessions) and sessions[session] == entry
        end_index = (session + MONTHLY_LABEL_SESSIONS) if aligned and session is not None else None
        label_end = sessions[end_index] if end_index is not None and end_index < len(sessions) else None
        matured = label_end is not None and (dt.datetime.combine(label_end, dt.time())
                                             + dt.timedelta(days=1, hours=12)) <= spec.label_cutoff
        following = (day.year + day.month // 12, day.month % 12 + 1)
        hold_end = month_last.get(following)
        following_formation = next_formed[day]
        if hold_end is not None and following_formation is not None:
            hold_end = min(hold_end, following_formation)   # holds never overlap (M10)
        formations.append(_Formation(int(row.month_index), day, pd.Timestamp(row.cutoff).to_pydatetime(), entry,
                                     session, aligned, label_end, matured,
                                     hold_end if hold_end is not None and hold_end > day else None))

    # Context: the price-line universe (valid primary + eligible unlinked lines), verified cap, PIT venue.
    verified = str(_evaluation._panel_run(store, table, False)["size_verified_status"])
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _p4_context AS
        WITH cap AS (
            SELECT formation_date, security_id,
                   max(raw_value) FILTER (WHERE size_status = ?) AS market_cap,
                   bool_or(size_status IS DISTINCT FROM ?) AS unverified_cap,
                   max(raw_value) FILTER (WHERE size_status IS DISTINCT FROM ?) AS unverified_value
            FROM research_panel_values
            WHERE run_id=? AND metric_code='market_cap' AND metric_window='daily' AND reason='valid'
              AND raw_value > 0 AND isfinite(raw_value)
            GROUP BY ALL
        ), members AS (
            SELECT m.month_index, c.security_id, s.code AS security,
                   bool_or(coalesce(c.eligible, false) AND c.cohort_reason = 'valid') AS valid_member,
                   bool_or(coalesce(c.primary_line, false)) AS primary_line,
                   bool_or(coalesce(c.eligible, false) AND list_contains(?::VARCHAR[], c.cohort_reason)) AS unlinked,
                   max(cap.market_cap) AS market_cap, coalesce(bool_or(cap.unverified_cap), false) AS unverified_cap,
                   max(cap.unverified_value) AS unverified_value,
                   bool_or(p.security IS NOT NULL) AS venue_pit,
                   bool_or(coalesce(p.exchange_code = ?, false)) AS nyse
            FROM research_panel_cohort c
            JOIN _ev_months m ON m.formation_date = c.formation_date
            JOIN _ev_securities s ON s.security_id = c.security_id
            LEFT JOIN cap ON cap.formation_date = c.formation_date AND cap.security_id = c.security_id
            LEFT JOIN _ev_pit_venue p ON p.month_index = m.month_index AND p.security = s.code
            WHERE c.run_id = ?
            GROUP BY m.month_index, c.security_id, s.code
        )
        SELECT month_index, security_id, security, valid_member AND primary_line AS linked,
               unlinked AND NOT (valid_member AND primary_line) AS unlinked,
               CASE WHEN valid_member AND primary_line THEN market_cap END AS market_cap,
               valid_member AND primary_line AND market_cap IS NULL AND unverified_cap AS unverified_cap,
               -- measurement only (excluded_cap_share), never a weight
               CASE WHEN valid_member AND primary_line AND market_cap IS NULL THEN unverified_value END
                 AS unverified_value,
               venue_pit, nyse
        FROM members WHERE (valid_member AND primary_line) OR unlinked
    """, [verified, verified, verified, table.panel_run_id, list(OWNER_LINK_FAILURES), NYSE_EXCHANGE_CODE,
          table.panel_run_id])
    diag: dict[str, Any] = {"size_verified_status": verified}

    # Sort characteristics: R2b raw values of in-domain rows, never after the formation cutoff.
    wanted = sorted({feature for feature, _, _ in SORTS.values()})
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _p4_chars AS
        SELECT m.month_index, s.code AS security, x.feature_id, x.raw_value AS value, x.available_at > m.cutoff AS late
        FROM research_feature_matrix x
        JOIN _ev_months m ON m.formation_date = x.formation_date
        JOIN _ev_securities s ON s.security_id = x.security_id
        WHERE x.feature_version = ? AND list_contains(?::VARCHAR[], x.feature_id) AND x.domain_status = ?
          AND x.raw_value IS NOT NULL AND isfinite(x.raw_value)
    """, [table.feature_version, wanted, IN_DOMAIN])
    late = con.execute("SELECT count(*), any_value(feature_id) FROM _p4_chars WHERE late").fetchone()
    if late and late[0]:
        raise FactorLookaheadError(f"{late[0]} R2b characteristic values (e.g. {late[1]}) are available after "
                                   "their formation cutoff")
    present = tuple(row[0] for row in con.execute("SELECT DISTINCT feature_id FROM _p4_chars ORDER BY 1").fetchall())

    # Industry groups per taxonomy (linked lines only: an unlinked line has no owner classification).
    con.execute("CREATE OR REPLACE TEMP TABLE _p4_industry (month_index BIGINT, security BIGINT, taxonomy VARCHAR, "
                "industry_group VARCHAR)")
    taxonomies = []
    for source in (table, *industry_tables):
        taxonomy = _taxonomy(source.classification_basis)
        if any(taxonomy == t for _, t, _ in taxonomies):
            raise FactorInputError(f"two feature versions carry the same industry taxonomy {taxonomy}")
        taxonomies.append((source.feature_version, taxonomy, str(source.classification_basis)))
        con.execute("""
            INSERT INTO _p4_industry
            SELECT m.month_index, s.code, ?, x.industry_group
            FROM research_feature_context x
            JOIN _ev_months m ON m.formation_date = x.formation_date
            JOIN _ev_securities s ON s.security_id = x.security_id
            WHERE x.feature_version = ? AND x.industry_group IS NOT NULL
              AND coalesce(x.owner_basis, ?) = ?
        """, [taxonomy, source.feature_version, OWNER_BASIS_LINKED, OWNER_BASIS_LINKED])

    # Delisting terminals (R3a selection and halt-gap dating) of every cohort security.
    cutoff = spec.label_cutoff
    if store.warehouse_has("delisting_terminal_returns"):
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _p4_term_sel AS
            SELECT t.* FROM ({selected_terminals_sql()}) t JOIN _ev_securities s ON s.security_id = t.security_id
        """, [cutoff, cutoff])
        term_filter = ("EXISTS (SELECT 1 FROM _p4_term_sel t WHERE t.security_id = equity_daily_bars.security_id "
                       "AND equity_daily_bars.trade_date < t.delist_date "
                       "AND equity_daily_bars.trade_date >= t.delist_date - INTERVAL 120 DAY)")
        con.execute(f"CREATE OR REPLACE TEMP TABLE _p4_term_bars AS {selected_bars_sql('adjusted_close', security_filter=term_filter)}",
                    [cutoff, cutoff])
        con.execute(f"CREATE OR REPLACE TEMP TABLE _p4_terms AS "
                    f"{effective_terminals_sql(terminals='_p4_term_sel', bars='_p4_term_bars', calendar='_p4_sessions')}")
    else:
        con.execute("CREATE OR REPLACE TEMP TABLE _p4_terms (security_id VARCHAR, delist_date DATE, "
                    "terminal_return DOUBLE, terminal_available_at TIMESTAMP, terminal_valid BOOLEAN, "
                    "last_price_date DATE, last_price_available_at TIMESTAMP, effective_delist_date DATE)")
    diag["terminals"] = int(con.execute("SELECT count(*) FROM _p4_terms").fetchone()[0])

    # Accumulators for the exposure pass.
    con.execute("CREATE OR REPLACE TEMP TABLE _p4_mret (month_index BIGINT, security BIGINT, ret DOUBLE, "
                "available_at TIMESTAMP, window_end DATE)")
    con.execute("CREATE OR REPLACE TEMP TABLE _p4_mf (month_index BIGINT, window_end DATE, factor_id VARCHAR, "
                "value DOUBLE, available_at TIMESTAMP, venue_basis VARCHAR)")
    con.execute("CREATE OR REPLACE TEMP TABLE _p4_mkt_daily (trade_date DATE, session_number BIGINT, "
                "mkt_rf DOUBLE, rf DOUBLE, available_at TIMESTAMP)")
    has_market_daily = all(store.warehouse_has("market_daily_metrics", column) for column in (
        "market_cap", "shares_source", "available_at", "as_of_date", "market_daily_id"))
    return _Work(table.basis, formations, sessions, number, taxonomies, present, MARKET_SOURCES[table.basis],
                 has_market_daily, pit_membership, diag)


# ---------------------------------------------------------------------------
# Monthly: market, 2x3 style factors, industries, breakpoints, segments
# ---------------------------------------------------------------------------

def _horizon_window(f: _Formation, horizon_months: int, work: _Work, spec: FactorSpec) -> _Formation:
    """``f`` with the R3a h-month label window: entry + 21h sessions, matured by the cutoff rule."""
    if not f.entry_aligned or f.session is None:
        return replace(f, label_end=None, label_matured=False)
    end_index = f.session + MONTHLY_LABEL_SESSIONS * int(horizon_months)
    end = work.sessions[end_index] if end_index < len(work.sessions) else None
    matured = end is not None and (dt.datetime.combine(end, dt.time()) + dt.timedelta(days=1, hours=12)
                                   ) <= spec.label_cutoff
    return replace(f, label_end=end, label_matured=matured)


def _monthly_labels(con: duckdb.DuckDBPyConnection, spec: FactorSpec, formations: Sequence[_Formation],
                    horizon_sessions: int = MONTHLY_LABEL_SESSIONS) -> pd.DataFrame:
    """R3a labels (21 sessions, or ``horizon_sessions`` with each formation's ``label_end`` at
    that horizon) of every cohort security at each formation, as R3b reads them (revision
    first, then the FQ2 validity fragment at the observation cutoff)."""
    windows = [f for f in formations if f.entry_aligned and f.label_end is not None]
    if not windows:
        return pd.DataFrame({"month_index": pd.Series(dtype="int64"), "security": pd.Series(dtype="int64"),
                             "ret": pd.Series(dtype=float), "status": pd.Series(dtype=object),
                             "available_at": pd.Series(dtype="datetime64[us]")})
    _stage_frame(con, "_p4_lwin", pd.DataFrame({
        "month_index": [f.month_index for f in windows], "entry_date": [f.entry_date for f in windows],
        "expected_end": [f.label_end for f in windows]}),
        (("month_index", "BIGINT"), ("entry_date", "DATE"), ("expected_end", "DATE")))
    status_sql = label_status_sql(label="l", calculation_version="?", expected_end="w.expected_end",
                                  entry="w.entry_date", cutoff="?")
    return con.execute(f"""
        WITH revisions AS (
            SELECT l.*, row_number() OVER (
                PARTITION BY l.security_id, l.as_of_date, l.horizon_days
                ORDER BY {label_revision_order_sql("l")}) AS revision
            FROM forward_returns_survivorship_safe l
            JOIN (SELECT DISTINCT entry_date FROM _p4_lwin) k ON k.entry_date = l.as_of_date
            JOIN _ev_securities s ON s.security_id = l.security_id
            WHERE l.source = ? AND l.horizon_days = ? AND l.available_at <= ?
        )
        SELECT w.month_index, s.code AS security, l.forward_return AS ret, {status_sql} AS status,
               l.available_at
        FROM revisions l
        JOIN _p4_lwin w ON w.entry_date = l.as_of_date
        JOIN _ev_securities s ON s.security_id = l.security_id
        WHERE l.revision = 1
    """, [spec.label_source, int(horizon_sessions), spec.label_cutoff, LABEL_CALCULATION_VERSION,
          spec.label_cutoff]).df()


def _chunk_frame(con: duckdb.DuckDBPyConnection, months: list[int], labels: pd.DataFrame,
                 work: _Work) -> pd.DataFrame:
    frame = con.execute("""
        SELECT month_index, security, security_id, linked, unlinked, market_cap, unverified_cap, unverified_value,
               venue_pit, nyse
        FROM _p4_context WHERE list_contains(?::BIGINT[], month_index) ORDER BY month_index, security
    """, [months]).df()
    chars = con.execute("SELECT month_index, security, feature_id, value FROM _p4_chars "
                        "WHERE list_contains(?::BIGINT[], month_index)", [months]).df()
    if len(chars):
        wide = chars.pivot_table(index=["month_index", "security"], columns="feature_id", values="value",
                                 aggfunc="max").reset_index()
        frame = frame.merge(wide, on=["month_index", "security"], how="left")
    industries = con.execute("SELECT month_index, security, taxonomy, industry_group FROM _p4_industry "
                             "WHERE list_contains(?::BIGINT[], month_index)", [months]).df()
    for _, taxonomy, _ in work.taxonomies:
        part = industries[industries["taxonomy"] == taxonomy]
        part = part.drop_duplicates(["month_index", "security"]).rename(
            columns={"industry_group": f"ind::{taxonomy}"})[["month_index", "security", f"ind::{taxonomy}"]]
        frame = frame.merge(part, on=["month_index", "security"], how="left")
    return _with_returns(frame, labels)


def _with_returns(frame: pd.DataFrame, labels: pd.DataFrame) -> pd.DataFrame:
    """``frame`` with ``ret``/``ret_at`` from the valid rows of ``labels`` (replacing any)."""
    frame = frame.drop(columns=[c for c in ("ret", "ret_at") if c in frame.columns])
    valid = labels[labels["status"] == "valid"][["month_index", "security", "ret", "available_at"]]
    return frame.merge(valid.rename(columns={"available_at": "ret_at"}), on=["month_index", "security"], how="left")


def horizon_frequency(horizon_months: int) -> str:
    """``frequency`` of the exact h-month factor rows (R3a h-month label windows)."""
    return FREQ_MONTHLY if horizon_months == 1 else f"{FREQ_MONTHLY}_h{int(horizon_months)}"


def _market_row(base: dict[str, Any], factor: str, value: float, **extra: Any) -> dict[str, Any]:
    row = dict(base)
    row.update(factor_family=FAMILY_MARKET, factor_id=factor, value=_float(value),
               status=STATUS_COMPUTED if _float(value) is not None else "no_names")
    row.update(extra)
    return row


def _monthly_formation(f: _Formation, part: pd.DataFrame, spec: FactorSpec, work: _Work, rf_m: float,
                       rf_basis: str, *, frequency: str = FREQ_MONTHLY, structural: bool = True
                       ) -> dict[str, list[dict[str, Any]]]:
    """Every monthly row of one formation (numpy over the formation's context rows).

    With ``frequency`` = an h-month frequency, ``f`` carries the h-month label window and
    ``part`` the h-month label returns: the same formation portfolios (they depend only on
    caps and characteristics) earn their exact h-month returns. ``structural=False`` skips
    the breakpoint and segment rows (written once, by the 1-month pass).
    """
    security_id = part["security_id"].to_numpy(dtype=object)
    linked, unlinked = _bools(part, "linked"), _bools(part, "unlinked")
    universe = linked | unlinked
    cap = _floats(part, "market_cap")
    verified = linked & np.isfinite(cap) & (cap > 0)
    unverified = _bools(part, "unverified_cap") & linked & ~verified
    nyse = _bools(part, "nyse")
    ret = _floats(part, "ret")
    ret_at = _stamps(part, "ret_at")
    labeled = np.isfinite(ret)
    weight = np.where(verified, cap, 0.0)
    base = {"run_id": spec.run_id, "frequency": frequency, "period_date": f.formation_date,
            "window_start": f.entry_date, "window_end": f.label_end, "basis": work.basis, "rf_basis": rf_basis}
    out: dict[str, list[dict[str, Any]]] = {"returns": [], "breakpoints": [], "segments": [], "mf": []}
    matured = f.label_matured
    unverified_value = _floats(part, "unverified_value")
    excluded_cap = np.where(unverified & np.isfinite(unverified_value), unverified_value, 0.0)
    known_cap = np.where(verified, cap, excluded_cap)

    def counts(held: np.ndarray, weighted: np.ndarray | None) -> dict[str, Any]:
        """The module's count semantics (docstring "Counts on a return row")."""
        names = held & labeled
        used = names if weighted is None else names & weighted
        total = float(known_cap[names & linked].sum())
        share = float(excluded_cap[names & linked].sum() / total) if weighted is not None and total > 0 else None
        return {"n_held": int(held.sum()), "n_names": int(names.sum()), "n_weighted": int(used.sum()),
                "n_excluded_unverified": int((names & ~used & unverified).sum()),
                "n_excluded_no_weight": int((names & ~used & ~unverified).sum()),
                "n_unlabeled": int((held & ~labeled).sum()), "excluded_cap_share": share}

    if matured:
        common = {"venue_basis": VENUE_NOT_APPLICABLE}
        ew_mask = universe & labeled
        ew = float(ret[ew_mask].mean()) if ew_mask.any() else _NAN
        vw = _vw(ret, weight, verified & labeled)
        vw_counts = {**counts(universe, verified), "available_at": _max_stamp(ret_at, verified & labeled),
                     "weighting": WEIGHT_FORMATION_CAP, "universe_scope": SCOPE_VERIFIED, **common}
        out["returns"].append(_market_row(base, "mkt_ew", ew, **counts(universe, None),
                                          available_at=_max_stamp(ret_at, ew_mask), weighting=WEIGHT_EQUAL,
                                          universe_scope=SCOPE_ALL_LINES, **common))
        mkt_vw = _market_row(base, "mkt_vw", vw, **vw_counts)
        if mkt_vw["value"] is None:
            mkt_vw["status"] = "no_weighted_names"
        out["returns"].append(mkt_vw)
        rf_value = None if rf_basis == RF_NONE else _float(rf_m)
        rf_row = _market_row(base, "rf", _NAN if rf_value is None else rf_value, n_held=0, n_names=0, n_weighted=0,
                             available_at=f.cutoff, weighting="none", universe_scope=VENUE_NOT_APPLICABLE, **common)
        rf_row["status"] = (STATUS_RF_NONE if rf_basis == RF_NONE else
                            STATUS_COMPUTED if rf_value is not None else STATUS_NO_RF)
        out["returns"].append(rf_row)
        excess = vw - rf_m if math.isfinite(vw) and math.isfinite(rf_m) else _NAN
        mkt_rf = _market_row(base, "mkt_rf", excess, **vw_counts)
        if mkt_rf["value"] is None:
            mkt_rf["status"] = "no_weighted_names" if not math.isfinite(vw) else STATUS_NO_RF
        out["returns"].append(mkt_rf)
        out["mf"] += [{"factor_id": "rf", "value": rf_value, "available_at": f.cutoff,
                       "venue_basis": VENUE_NOT_APPLICABLE},
                      {"factor_id": "mkt_rf", "value": _float(excess), "available_at": vw_counts["available_at"],
                       "venue_basis": VENUE_NOT_APPLICABLE}]

    # Breakpoints on the valid cohort with a verified cap (sorts) and the A3 segments.
    size_ref, size_venue = reference_names(cap, verified, nyse, spec.nyse_min_names)
    variables: dict[str, tuple[np.ndarray, np.ndarray | None, str]] = {SIZE_FEATURE: (cap, size_ref, size_venue)}
    for feature in sorted({feature for feature, _, _ in SORTS.values()}):
        values = _floats(part, feature) if feature in part.columns else np.full(len(part), _NAN)
        reference, venue = reference_names(values, verified, nyse, spec.nyse_min_names)
        variables[feature] = (values, reference, venue)
    for name, (values, reference, venue) in (variables.items() if structural else ()):
        cuts = (np.percentile(values[reference], [20.0, 30.0, 50.0, 70.0]) if reference is not None
                else np.full(4, _NAN))
        out["breakpoints"].append({
            "run_id": spec.run_id, "formation_date": f.formation_date, "variable": name, "venue_basis": venue,
            "reference_names": int(reference.sum()) if reference is not None else int((verified & nyse & np.isfinite(values)).sum()),
            "universe_names": int((verified & np.isfinite(values)).sum()),
            "p20": _float(cuts[0]), "p30": _float(cuts[1]), "p50": _float(cuts[2]), "p70": _float(cuts[3]),
            "basis": work.basis, "rf_basis": rf_basis})

    # A3: NYSE-breakpoint size segments and cap-rank segments of every universe line.
    top, total = spec.rank_segments
    segment = np.full(len(part), "unknown", dtype=object)
    if size_ref is not None:
        groups = assign_groups(cap, np.percentile(cap[size_ref], list(SEGMENT_PERCENTILES)), verified, ties="upper")
        segment[groups > 0] = np.array(SIZE_SEGMENTS, dtype=object)[groups[groups > 0] - 1]
    order = np.lexsort((security_id.astype(str), -np.where(verified, cap, -np.inf)))
    rank = np.zeros(len(part), dtype=np.int64)
    ranked = order[verified[order]]
    rank[ranked] = np.arange(1, len(ranked) + 1)
    rank_segment = np.where(~verified, "unknown", np.where(rank <= top, f"top_{top}", np.where(
        rank <= total, f"next_{total - top}", f"beyond_{total}")))
    for i in (np.flatnonzero(universe) if structural else ()):
        out["segments"].append({
            "run_id": spec.run_id, "formation_date": f.formation_date, "security_id": security_id[i],
            "owner_basis": OWNER_BASIS_LINKED if linked[i] else OWNER_BASIS_UNLINKED,
            "market_cap": _float(cap[i]) if verified[i] else None, "size_segment": segment[i],
            "cap_rank": int(rank[i]) if verified[i] else None, "rank_segment": str(rank_segment[i]),
            "basis": work.basis, "venue_basis": size_venue, "rf_basis": rf_basis})

    if not matured:
        return out

    # 2x3 sorts: size (NYSE median) x characteristic (NYSE 30/70), value-weighted by verified cap.
    size_group = (assign_groups(cap, np.percentile(cap[size_ref], [SIZE_PERCENTILE]), verified, ties="upper")
                  if size_ref is not None else np.zeros(len(part), dtype=np.int8))
    smb_legs: dict[str, float] = {}
    style_at: dict[str, dt.datetime | None] = {}
    venues: dict[str, str] = {}
    for factor, (feature, long_group, short_group) in SORTS.items():
        values, reference, char_venue = variables[feature]
        venue = _combine_venues(size_venue, char_venue)
        venues[factor] = venue
        detail: dict[str, Any] = {"characteristic": feature, "size_venue": size_venue, "char_venue": char_venue,
                                  "long_group": long_group, "short_group": short_group, "rebalance": REBALANCE,
                                  **FACTOR_CONSTRUCTS[factor]}
        # Held: linked lines with the characteristic; weighted: those with a verified cap.
        row = dict(base, factor_family=FAMILY_STYLE, factor_id=factor, venue_basis=venue,
                   weighting=WEIGHT_FORMATION_CAP, universe_scope=SCOPE_VERIFIED, value=None,
                   **counts(linked & np.isfinite(values), verified))
        if feature not in work.characteristics:
            row["status"] = "characteristic_not_in_feature_version"
        elif reference is None or size_ref is None:
            row["status"] = "no_breakpoints"
        else:
            char_group = assign_groups(values, np.percentile(values[reference], list(SORT_PERCENTILES)), verified)
            portfolio: dict[tuple[int, int], float] = {}
            portfolio_legs: dict[str, list[Any]] = {}
            members_all = np.zeros(len(part), dtype=bool)
            for size in (1, 2):
                for group in (1, 2, 3):
                    members = (size_group == size) & (char_group == group)
                    used = members & labeled
                    members_all |= used
                    portfolio[(size, group)] = _vw(ret, weight, used)
                    portfolio_legs[f"{'SB'[size - 1]}{group}"] = [int(members.sum()), int(used.sum()),
                                                          _float(portfolio[(size, group)])]
            detail["portfolios"] = portfolio_legs
            thin = [key for key, (_, used, _) in portfolio_legs.items() if used < spec.min_portfolio_names]
            if thin:
                row["status"] = "thin_portfolio:" + ",".join(thin)
            else:
                long_leg = 0.5 * (portfolio[(1, long_group)] + portfolio[(2, long_group)])
                short_leg = 0.5 * (portfolio[(1, short_group)] + portfolio[(2, short_group)])
                row.update(value=_float(long_leg - short_leg), status=STATUS_COMPUTED)
                smb_legs[factor] = float(np.mean([portfolio[(1, g)] for g in (1, 2, 3)])
                                         - np.mean([portfolio[(2, g)] for g in (1, 2, 3)]))
                style_at[factor] = _max_stamp(ret_at, members_all)
                row["available_at"] = style_at[factor]
        row["detail_json"] = _canonical(detail)
        out["returns"].append(row)
    smb_ready = all(factor in smb_legs for factor in SMB_SORTS)
    smb_at = [style_at[f] for f in SMB_SORTS if style_at.get(f) is not None]
    for factor, value, legs in (("smb", float(np.mean([smb_legs[f] for f in SMB_SORTS])) if smb_ready else None,
                                 SMB_SORTS),
                                ("smb_ff3", smb_legs.get("hml"), ("hml",))):
        venue = _combine_venues(*(venues[f] for f in legs))
        at = (max(smb_at) if smb_at else None) if factor == "smb" else style_at.get("hml")
        out["returns"].append(dict(
            base, factor_family=FAMILY_STYLE, factor_id=factor, value=_float(value), venue_basis=venue,
            weighting=WEIGHT_FORMATION_CAP, universe_scope=SCOPE_VERIFIED, available_at=at,
            status=STATUS_COMPUTED if value is not None else "component_sort_not_formed",
            detail_json=_canonical({"size_legs_of": list(legs), "rebalance": REBALANCE,
                                    **FACTOR_CONSTRUCTS[factor]})))
    for row in out["returns"]:
        if row["factor_family"] == FAMILY_STYLE and row["factor_id"] in EXPOSURE_FACTORS:
            out["mf"].append({"factor_id": row["factor_id"], "value": row["value"],
                              "available_at": row.get("available_at"), "venue_basis": row["venue_basis"]})

    # Industries: value-weighted by verified formation cap, per taxonomy group.
    for _, taxonomy, classification in work.taxonomies:
        column = f"ind::{taxonomy}"
        if column not in part.columns:
            continue
        groups = part[column].to_numpy(dtype=object)
        for group in sorted({g for g in groups[linked] if isinstance(g, str)}):
            members = linked & (groups == group)
            used = members & verified & labeled
            value = _vw(ret, weight, used)
            out["returns"].append(dict(
                base, factor_family=FAMILY_INDUSTRY, factor_id=f"ind_{_slug(taxonomy)}_{_slug(group)}",
                value=_float(value), **counts(members, verified), available_at=_max_stamp(ret_at, used),
                venue_basis=VENUE_NOT_APPLICABLE, weighting=WEIGHT_FORMATION_CAP, universe_scope=SCOPE_VERIFIED,
                status=STATUS_COMPUTED if _float(value) is not None else "no_weighted_names",
                detail_json=_canonical({"taxonomy": taxonomy, "classification_basis": classification,
                                        "group": group})))
    return out


# ---------------------------------------------------------------------------
# Daily market (prior-session verified cap weights)
# ---------------------------------------------------------------------------

def _stage_bars(con: duckdb.DuckDBPyConnection, name: str, securities: str, low: dt.date, high: dt.date,
                cutoff: dt.datetime) -> None:
    """``name``: selected bars (label publisher pick) on calendar sessions with the previous
    bar of the same security (``start_*``), for the securities of relation ``securities``."""
    selection = selected_bars_sql("adjusted_close", security_filter=(
        f"security_id IN (SELECT security_id FROM {securities}) "
        f"AND trade_date BETWEEN DATE '{low.isoformat()}' AND DATE '{high.isoformat()}'"))
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE {name} AS
        WITH b AS (
            SELECT b.security_id, b.trade_date, s.session_number, b.price, b.price_available_at
            FROM ({selection}) b JOIN _p4_sessions s ON s.trade_date = b.trade_date
        )
        SELECT security_id, trade_date, session_number, price, price_available_at,
               lag(price) OVER w AS start_price, lag(trade_date) OVER w AS start_date,
               lag(session_number) OVER w AS start_session, lag(price_available_at) OVER w AS start_at
        FROM b WINDOW w AS (PARTITION BY security_id ORDER BY trade_date)
    """, [cutoff, cutoff])


def _daily_chunk(store: ResearchStore, spec: FactorSpec, work: _Work, chunk: Sequence[_Formation],
                 rf: RiskFreeSeries) -> list[dict[str, Any]]:
    con = store.con
    holds = [f for f in chunk if f.hold_end is not None]
    if not holds:
        return []
    _stage_frame(con, "_p4_holds", pd.DataFrame({
        "month_index": [f.month_index for f in holds], "formation_date": [f.formation_date for f in holds],
        "hold_end": [f.hold_end for f in holds]}),
        (("month_index", "BIGINT"), ("formation_date", "DATE"), ("hold_end", "DATE")))
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _p4_hold AS
        SELECT c.month_index, c.security_id, c.linked, h.formation_date, h.hold_end
        FROM _p4_context c JOIN _p4_holds h ON h.month_index = c.month_index
    """)
    con.execute("CREATE OR REPLACE TEMP TABLE _p4_hold_ids AS SELECT DISTINCT security_id FROM _p4_hold")
    low = min(f.formation_date for f in holds) - dt.timedelta(days=spec.lookback_calendar_days)
    high = max(f.hold_end for f in holds if f.hold_end is not None)
    _stage_bars(con, "_p4_bars", "_p4_hold_ids", low, high, spec.label_cutoff)
    held = dict(con.execute("SELECT month_index, count(*) FROM _p4_hold GROUP BY month_index").fetchall())
    # Bar returns (prints on/after a terminal's effective date are dropped) and terminal returns.
    # A bar return counts only when its start price lies within the lookback before the hold's own
    # formation (a per-hold bound, so the chunking never changes a result, M4); a terminal row's
    # start is the run-level last trade before delisting and is exempt (a delisting loss never drops).
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _p4_held_ret AS
        WITH ret AS (
            SELECT b.security_id, b.trade_date, b.session_number, b.start_date, b.start_session,
                   b.price / b.start_price - 1 AS ret, greatest(b.price_available_at, b.start_at) AS available_at,
                   false AS terminal
            FROM _p4_bars b LEFT JOIN _p4_terms t ON t.security_id = b.security_id
            WHERE b.start_price IS NOT NULL AND (t.effective_delist_date IS NULL OR b.trade_date < t.effective_delist_date)
            UNION ALL
            SELECT t.security_id, t.effective_delist_date, s.session_number, t.last_price_date, ls.session_number,
                   t.terminal_return, greatest(t.terminal_available_at, t.last_price_available_at), true
            FROM _p4_terms t
            JOIN _p4_hold_ids h ON h.security_id = t.security_id
            JOIN _p4_sessions s ON s.trade_date = t.effective_delist_date
            LEFT JOIN _p4_sessions ls ON ls.trade_date = t.last_price_date
            WHERE t.terminal_valid
        )
        SELECT h.month_index, h.linked, r.*
        FROM ret r JOIN _p4_hold h
          ON h.security_id = r.security_id AND r.trade_date > h.formation_date AND r.trade_date <= h.hold_end
         AND (r.terminal OR r.start_date >= h.formation_date - INTERVAL {int(spec.lookback_calendar_days)} DAY)
    """)
    starts = con.execute("SELECT min(start_date), max(start_date) FROM _p4_held_ret").fetchone()
    if work.has_market_daily and starts is not None and starts[0] is not None:
        # The cap at exactly each return's start-price session (the date range only prunes).
        con.execute("""
            CREATE OR REPLACE TEMP TABLE _p4_caps AS
            SELECT m.security_id, m.trade_date,
                   arg_max(struct_pack(cap := m.market_cap, src := m.shares_source, clock := m.available_at),
                           (m.available_at, m.market_daily_id)) AS pick
            FROM market_daily_metrics m
            JOIN (SELECT DISTINCT security_id, start_date FROM _p4_held_ret WHERE start_date IS NOT NULL) k
              ON k.security_id = m.security_id AND k.start_date = m.trade_date
            WHERE m.source = ? AND m.trade_date BETWEEN ? AND ?
              AND m.available_at <= CAST(m.trade_date AS TIMESTAMP) + INTERVAL 22 HOUR AND m.as_of_date <= m.trade_date
            GROUP BY ALL
        """, [work.market_source, starts[0], starts[1]])
    else:
        con.execute("CREATE OR REPLACE TEMP TABLE _p4_caps (security_id VARCHAR, trade_date DATE, "
                    "pick STRUCT(cap DOUBLE, src VARCHAR, clock TIMESTAMP))")
    days = con.execute("""
        WITH joined AS (
            SELECT r.*, struct_extract(c.pick, 'cap') AS cap, struct_extract(c.pick, 'clock') AS cap_at,
                   coalesce(struct_extract(c.pick, 'cap') > 0 AND isfinite(struct_extract(c.pick, 'cap')), false)
                     AS has_cap,
                   coalesce(list_contains(?::VARCHAR[], struct_extract(c.pick, 'src')), false) AS dei
            FROM _p4_held_ret r
            LEFT JOIN _p4_caps c ON c.security_id = r.security_id AND c.trade_date = r.start_date
        ), flagged AS (SELECT *, linked AND has_cap AND dei AS weighted FROM joined)
        SELECT trade_date, any_value(session_number) AS session_number, min(month_index) AS month_index,
               count(DISTINCT month_index) AS n_formations, count(DISTINCT security_id) AS n_securities,
               -- ordered sums: floating-point results never depend on staging row order (chunking)
               count(*) AS n_names, avg(ret ORDER BY security_id) AS ew,
               count(*) FILTER (WHERE weighted) AS n_weighted,
               sum(cap * ret ORDER BY security_id) FILTER (WHERE weighted)
                 / sum(cap ORDER BY security_id) FILTER (WHERE weighted) AS vw,
               count(*) FILTER (WHERE linked AND has_cap AND NOT dei) AS n_unverified,
               count(*) FILTER (WHERE NOT weighted AND NOT (linked AND has_cap AND NOT dei)) AS n_no_weight,
               sum(cap ORDER BY security_id) FILTER (WHERE linked AND has_cap AND NOT dei) AS cap_unverified,
               sum(cap ORDER BY security_id) FILTER (WHERE linked AND has_cap) AS cap_known,
               count(*) FILTER (WHERE terminal) AS n_terminal,
               count(*) FILTER (WHERE start_session IS DISTINCT FROM session_number - 1) AS n_gap,
               max(available_at) AS ew_at,
               max(greatest(available_at, cap_at)) FILTER (WHERE weighted) AS vw_at
        FROM flagged GROUP BY trade_date ORDER BY trade_date
    """, [list(VERIFIED_SHARES_SOURCES)]).df()
    rows: list[dict[str, Any]] = []
    if not len(days):
        return rows
    if (days["n_formations"] > 1).any() or (days["n_securities"] != days["n_names"]).any():
        raise FactorInputError("overlapping holds: a trade date belongs to more than one formation or a name "
                               "has two returns on one day")
    session = days["session_number"].to_numpy(dtype=np.int64)
    prior = np.array([work.sessions[s - 2] if s >= 2 else work.sessions[0] for s in session], dtype="datetime64[D]")
    rf_d = rf.period_returns(prior, 1)
    mkt_rows = []
    for i, day in enumerate(days.itertuples(index=False)):
        trade_date = pd.Timestamp(day.trade_date).date()
        n_held = int(held.get(int(day.month_index), 0))
        cap_known = _float(day.cap_known)
        share = (float(_float(day.cap_unverified) or 0.0) / cap_known) if cap_known else None
        base = {"run_id": spec.run_id, "frequency": FREQ_DAILY, "period_date": trade_date,
                "window_start": pd.Timestamp(prior[i]).date(), "window_end": trade_date, "factor_family": FAMILY_MARKET,
                "basis": work.basis, "rf_basis": rf.basis, "venue_basis": VENUE_NOT_APPLICABLE,
                "n_held": n_held, "n_names": int(day.n_names), "n_unlabeled": n_held - int(day.n_names),
                "detail_json": _canonical({"n_terminal_returns": int(day.n_terminal), "n_gap_returns": int(day.n_gap),
                                           "formation_month_index": int(day.month_index)})}
        vw = _float(day.vw)
        weighted = {"n_weighted": int(day.n_weighted), "excluded_cap_share": share,
                    "n_excluded_unverified": int(day.n_unverified), "n_excluded_no_weight": int(day.n_no_weight),
                    "weighting": WEIGHT_PRIOR_DAY_CAP, "universe_scope": SCOPE_VERIFIED,
                    "available_at": _stamp(day.vw_at)}
        ew = _float(day.ew)
        rows.append(dict(base, factor_id="mkt_ew", value=ew, n_weighted=int(day.n_names),
                         n_excluded_unverified=0, n_excluded_no_weight=0, weighting=WEIGHT_EQUAL,
                         universe_scope=SCOPE_ALL_LINES, available_at=_stamp(day.ew_at),
                         status=STATUS_COMPUTED if ew is not None else "no_names"))
        rows.append(dict(base, factor_id="mkt_vw", value=vw, status=STATUS_COMPUTED if vw is not None
                         else "no_weighted_names", **weighted))
        rf_value = None if rf.basis == RF_NONE else _float(rf_d[i])
        rf_at = dt.datetime.combine(pd.Timestamp(prior[i]).date(), dt.time(22))
        rows.append(dict(base, factor_id="rf", value=rf_value, n_held=0, n_names=0, n_unlabeled=0, n_weighted=0,
                         n_excluded_unverified=0, n_excluded_no_weight=0, weighting="none",
                         universe_scope=VENUE_NOT_APPLICABLE, available_at=rf_at,
                         status=(STATUS_RF_NONE if rf.basis == RF_NONE else
                                 STATUS_COMPUTED if rf_value is not None else STATUS_NO_RF)))
        # rf_basis 'none': the excess return is the raw VW return (labeled on the row).
        rf_used = 0.0 if rf.basis == RF_NONE else rf_value
        excess = vw - rf_used if vw is not None and rf_used is not None else None
        rows.append(dict(base, factor_id="mkt_rf", value=excess, status=STATUS_COMPUTED if excess is not None
                         else "no_weighted_names" if vw is None else STATUS_NO_RF, **weighted))
        mkt_rows.append({"trade_date": trade_date, "session_number": int(session[i]), "mkt_rf": excess,
                         "rf": rf_used if rf_used is not None else 0.0,
                         "available_at": _stamp(day.vw_at)})
    _stage_frame(con, "_p4_unused", pd.DataFrame(mkt_rows), (
        ("trade_date", "DATE"), ("session_number", "BIGINT"), ("mkt_rf", "DOUBLE"), ("rf", "DOUBLE"),
        ("available_at", "TIMESTAMP")), into="_p4_mkt_daily")
    duplicated = con.execute("SELECT count(*) - count(DISTINCT trade_date) FROM _p4_mkt_daily").fetchone()
    if duplicated and duplicated[0]:
        raise FactorInputError(f"overlapping holds across chunks: {duplicated[0]} repeated daily market dates")
    diag = work.diag.setdefault("daily", {"days": 0, "terminal_returns": 0, "gap_returns": 0,
                                          "unverified_exclusions": 0, "no_weight_exclusions": 0})
    diag["days"] += len(days)
    diag["terminal_returns"] += int(days["n_terminal"].sum())
    diag["gap_returns"] += int(days["n_gap"].sum())
    diag["unverified_exclusions"] += int(days["n_unverified"].sum())
    diag["no_weight_exclusions"] += int(days["n_no_weight"].sum())
    return rows


# ---------------------------------------------------------------------------
# Exposures
# ---------------------------------------------------------------------------

def _exposures_chunk(store: ResearchStore, spec: FactorSpec, work: _Work, chunk: Sequence[_Formation],
                     rf_basis: str) -> pd.DataFrame:
    con = store.con
    frames = []
    window = spec.beta_window_sessions
    located = [f for f in chunk if f.session is not None]
    if located and spec.daily:
        first_session = max(min(f.session for f in located if f.session is not None) - window, 1)
        con.execute("CREATE OR REPLACE TEMP TABLE _p4_xids AS SELECT DISTINCT security_id FROM _p4_context "
                    "WHERE list_contains(?::BIGINT[], month_index)", [[f.month_index for f in located]])
        _stage_bars(con, "_p4_xbars", "_p4_xids", work.sessions[first_session - 1],
                    max(f.formation_date for f in located), spec.label_cutoff)
    for f in chunk:
        universe = con.execute("""
            SELECT security, security_id, linked FROM _p4_context
            WHERE month_index = ? AND (linked OR unlinked) ORDER BY security
        """, [f.month_index]).df()
        if not len(universe):
            continue
        out = pd.DataFrame({"security_id": universe["security_id"].astype(object),
                            "owner_basis": np.where(_bools(universe, "linked"), OWNER_BASIS_LINKED,
                                                    OWNER_BASIS_UNLINKED)})
        at = pd.Series([pd.NaT] * len(out), dtype="datetime64[us]")
        # Market model on the trailing daily window (same-session returns only).
        daily = pd.DataFrame()
        if f.session is not None and spec.daily:
            daily = con.execute("""
                WITH obs AS (
                    SELECT b.security_id, b.session_number, b.price / b.start_price - 1 - m.rf AS y, m.mkt_rf AS x,
                           greatest(b.price_available_at, b.start_at, m.available_at) AS clock
                    FROM _p4_xbars b
                    JOIN _p4_mkt_daily m ON m.session_number = b.session_number
                    JOIN (SELECT security_id FROM _p4_context WHERE month_index = ? AND (linked OR unlinked)) u
                      ON u.security_id = b.security_id
                    WHERE b.session_number BETWEEN ? AND ? AND b.start_session = b.session_number - 1
                      AND b.price_available_at <= ? AND m.available_at <= ? AND m.mkt_rf IS NOT NULL
                )
                SELECT security_id, regr_count(y, x) AS n, regr_slope(y, x ORDER BY session_number) AS beta,
                       regr_intercept(y, x ORDER BY session_number) AS alpha,
                       regr_r2(y, x ORDER BY session_number) AS r2, regr_sxx(y, x ORDER BY session_number) AS sxx,
                       regr_syy(y, x ORDER BY session_number) AS syy,
                       regr_sxy(y, x ORDER BY session_number) AS sxy, max(clock) AS clock
                FROM obs GROUP BY security_id
            """, [f.month_index, f.session - window + 1, f.session, f.cutoff, f.cutoff]).df()
        if len(daily):
            n = daily["n"].to_numpy(dtype=float)
            sxx, syy, sxy = (_floats(daily, c) for c in ("sxx", "syy", "sxy"))
            with np.errstate(divide="ignore", invalid="ignore"):
                sigma2 = np.maximum(syy - sxy * sxy / sxx, 0.0) / (n - 2)
                se = np.sqrt(sigma2 / sxx)
            ok = (n >= spec.beta_min_sessions) & (sxx > 0)
            daily = daily.assign(beta_mkt_252d=np.where(ok, _floats(daily, "beta"), _NAN),
                                 se_beta_mkt_252d=np.where(ok, se, _NAN),
                                 alpha_252d=np.where(ok, _floats(daily, "alpha"), _NAN),
                                 ivol_252d=np.where(ok, np.sqrt(sigma2 * SESSIONS_PER_YEAR), _NAN),
                                 r2_252d=np.where(ok, _floats(daily, "r2"), _NAN),
                                 n_daily_obs=daily["n"].astype("int64"),
                                 status_252d=np.where(ok, EXPOSURE_ESTIMATED, "insufficient_obs"),
                                 at_252=np.where(ok, _stamps(daily, "clock"), np.datetime64("NaT", "us")))
            out = out.merge(daily[["security_id", "beta_mkt_252d", "se_beta_mkt_252d", "alpha_252d", "ivol_252d",
                                   "r2_252d", "n_daily_obs", "status_252d", "at_252"]], on="security_id", how="left")
            at = pd.Series(out["at_252"].to_numpy(dtype="datetime64[us]"))
        out["n_daily_obs"] = out.get("n_daily_obs", pd.Series(0, index=out.index)).fillna(0).astype("int64")
        out["status_252d"] = out.get("status_252d", pd.Series(None, index=out.index, dtype=object)).fillna(
            "no_daily_observations" if spec.daily else "not_requested")

        # Six-factor betas on the trailing monthly windows that ended by the formation.
        first = f.month_index - spec.factor_window_months
        factors = con.execute(f"""
            SELECT month_index, max(window_end) AS window_end, max(available_at) AS clock,
                   {', '.join(f"max(value) FILTER (WHERE factor_id = '{name}') AS {name}" for name in ('rf', *EXPOSURE_FACTORS))},
                   max(venue_basis) FILTER (WHERE factor_id = 'hml') AS venue
            FROM _p4_mf WHERE month_index BETWEEN ? AND ? GROUP BY month_index
        """, [first, f.month_index - 1]).df()
        usable = factors[(pd.to_datetime(factors["window_end"]).dt.date <= f.formation_date)
                         & (pd.to_datetime(factors["clock"]) <= pd.Timestamp(f.cutoff))
                         & factors[list(EXPOSURE_FACTORS)].notna().all(axis=1)] if len(factors) else factors
        venue = _evaluation._method_counts(usable["venue"].fillna(VENUE_NONE).tolist(),
                                           np.ones(len(usable), dtype=bool)) if len(usable) else None
        monthly = pd.DataFrame()
        if len(usable):
            _stage_frame(con, "_p4_xfac", usable[["month_index", "rf", *EXPOSURE_FACTORS]], (
                ("month_index", "BIGINT"), ("rf", "DOUBLE"), *((name, "DOUBLE") for name in EXPOSURE_FACTORS)))
            monthly = con.execute(f"""
                SELECT u.row_index, r.ret - coalesce(x.rf, 0) AS y, {', '.join(f'x.{n}' for n in EXPOSURE_FACTORS)},
                       r.available_at AS clock
                FROM _p4_mret r
                JOIN _p4_xfac x ON x.month_index = r.month_index
                JOIN (SELECT security, row_number() OVER (ORDER BY security) - 1 AS row_index FROM _p4_context
                      WHERE month_index = ? AND (linked OR unlinked)) u ON u.security = r.security
                WHERE r.window_end <= ? AND r.available_at <= ?
                ORDER BY u.row_index, r.month_index
            """, [f.month_index, f.formation_date, f.cutoff]).df()
        if len(monthly):
            group = monthly["row_index"].to_numpy(dtype=np.int64)
            design = np.column_stack([np.ones(len(monthly)), monthly[list(EXPOSURE_FACTORS)].to_numpy(dtype=float)])
            fit = grouped_ols(group, _floats(monthly, "y"), design, len(out), spec.factor_min_months)
            estimated = fit["status"] == EXPOSURE_ESTIMATED
            out["alpha_36m"] = fit["beta"][:, 0]
            for j, name in enumerate(EXPOSURE_FACTORS, start=1):
                out[f"beta_{name}_36m"] = fit["beta"][:, j]
                out[f"se_{name}_36m"] = fit["se"][:, j]
            out["resid_vol_36m"] = fit["resid_sd"]
            out["r2_36m"] = fit["r2"]
            out["n_monthly_obs"] = fit["n"]
            out["status_36m"] = fit["status"]
            clock = pd.Series(_stamps(monthly, "clock")).groupby(group).max()
            at_36 = np.full(len(out), np.datetime64("NaT", "us"), dtype="datetime64[us]")
            at_36[clock.index.to_numpy()] = clock.to_numpy(dtype="datetime64[us]")
            factor_at = pd.to_datetime(usable["clock"]).max().to_datetime64()
            at_36 = np.where(estimated & ~np.isnat(at_36), np.maximum(at_36, factor_at), np.datetime64("NaT", "us"))
            at = pd.Series(np.fmax(at.to_numpy(dtype="datetime64[us]"), at_36))
        else:
            out["n_monthly_obs"] = 0
            out["status_36m"] = "no_factor_history"
        out["available_at"] = at.to_numpy()
        out["run_id"] = spec.run_id
        out["formation_date"] = f.formation_date
        out["basis"] = work.basis
        out["venue_basis"] = venue or VENUE_NOT_APPLICABLE
        out["rf_basis"] = rf_basis
        frames.append(out.drop(columns=[c for c in ("at_252",) if c in out.columns]))
    return pd.concat(frames, ignore_index=True) if frames else pd.DataFrame()


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class FactorRunResult:
    run_id: str
    status: str
    basis: str
    rf_basis: str
    rows: dict[str, int]
    blockers: tuple[str, ...]
    results_sha256: str | None
    diagnostic: dict[str, Any]


def results_digest(con: duckdb.DuckDBPyConnection, run_id: str) -> tuple[str, dict[str, str]]:
    """sha256 of every result table's rows of a run, streamed in their canonical order."""
    parts = {}
    for key, order in _ORDER.items():
        table, columns = _TABLES[key]
        digest = hashlib.sha256()
        cursor = con.execute(f"SELECT {', '.join(name for name, _ in columns if name != 'run_id')} FROM {table} "
                             f"WHERE run_id=? ORDER BY {order}", [run_id])
        while batch := cursor.fetchmany(4096):
            for row in batch:
                digest.update(_canonical([None if isinstance(v, float) and not math.isfinite(v) else v
                                          for v in row]).encode("utf-8"))
                digest.update(b"\n")
        parts[key] = digest.hexdigest()
    return _sha(_canonical(parts)), parts


def _delete_run(con: duckdb.DuckDBPyConnection, run_id: str) -> None:
    for table, _ in _TABLES.values():
        con.execute(f"DELETE FROM {table} WHERE run_id=?", [run_id])


def build_factor_returns(store: ResearchStore, spec: FactorSpec, *, risk_free: RiskFreeSeries | None = None,
                         replace_run: bool = False) -> FactorRunResult:
    """Build one factor run (see the module docstring) and seal it with a results digest.

    ``risk_free`` defaults to the cached DTB3 series (``rf_basis='none'`` without it). A
    completed run is never replaced unless ``replace_run``.
    """
    spec = validate_factor_spec(spec)
    rf = risk_free if risk_free is not None else load_risk_free()
    con = store.con
    ensure_factor_schema(con)
    existing = con.execute("SELECT status FROM research_factor_runs WHERE run_id=?", [spec.run_id]).fetchone()
    if existing is not None and existing[0] == "complete" and not replace_run:
        raise FactorInputError(f"factor run {spec.run_id} is complete; pass replace_run=True to rebuild it")
    table = _evaluation.load_feature_table(store, spec.feature_version)
    panel = _evaluation._panel_run(store, table, spec.verify_panels)
    if spec.verify_panels and table.status == "sealed":
        from .features import FeatureStoreError, validate_feature_version

        try:
            validate_feature_version(store, spec.feature_version, verify_panel=False)
        except FeatureStoreError as error:
            raise FactorInputError(f"R2b feature version {spec.feature_version} fails its validator: {error}") \
                from error
    industry_tables = []
    for version in spec.industry_versions:
        other = _evaluation.load_feature_table(store, version)
        if other.panel_run_id != table.panel_run_id:
            raise FactorInputError(f"industry version {version} is built on panel run {other.panel_run_id}, "
                                   f"not {table.panel_run_id}")
        industry_tables.append(other)
    payload = spec_payload(spec)
    payload.update(basis=table.basis, panel_run_id=table.panel_run_id, panel_sha256=table.panel_sha256,
                   feature_query_version=table.query_version, feature_catalog_sha256=table.catalog_sha256,
                   risk_free=rf.manifest())
    blockers = ["research_only_not_release_eligible", *(f"feature:{b}" for b in table.blockers)]
    if rf.basis == RF_NONE:
        blockers.append("rf_basis_none_excess_returns_equal_raw_returns")
    now = _now()
    with store.transaction():
        _delete_run(con, spec.run_id)
        _insert(con, "runs", [{
            "run_id": spec.run_id, "status": "building", "basis": table.basis,
            "feature_version": spec.feature_version, "panel_run_id": table.panel_run_id,
            "panel_sha256": table.panel_sha256, "factor_version": FACTOR_VERSION, "rf_basis": rf.basis,
            "rf_sha256": rf.sha256, "label_source": spec.label_source, "label_cutoff": spec.label_cutoff,
            "spec_json": _canonical(payload), "spec_sha256": _sha(_canonical(payload)), "code_sha256": _code_sha(),
            "blockers_json": _canonical(blockers), "diagnostic_json": None, "results_sha256": None,
            "created_at": now, "finished_at": None}])
    counts = dict.fromkeys(("returns", "breakpoints", "segments", "exposures"), 0)
    diagnostic: dict[str, Any] = {"panel_blockers": panel.get("panel_blockers"), "rf": rf.manifest()}
    try:
        if table.status == _evaluation.BASIS_UNTESTABLE or panel["panel_status"] == "untestable_strict":
            return _finish(store, spec, table.basis, rf.basis, "untestable_strict", counts, blockers, diagnostic)
        work = _stage(store, spec, table, industry_tables)
        diagnostic.update(work.diag, formed=len(work.formations), pit_membership_table=work.pit_membership,
                          taxonomies=[{"feature_version": v, "taxonomy": t, "classification_basis": c}
                                      for v, t, c in work.taxonomies],
                          characteristics_present=list(work.characteristics))
        if not work.has_market_daily and spec.daily:
            blockers.append("daily_vw_market_daily_metrics_absent")
        for feature in sorted({feature for feature, _, _ in SORTS.values()} - set(work.characteristics)):
            blockers.append(f"characteristic_not_in_feature_version:{feature}")
        if not any("49" in taxonomy for _, taxonomy, _ in work.taxonomies):
            blockers.append("ff49_industry_returns_not_built_needs_ff49_classification_version")
        venue_counts: dict[str, int] = {}
        horizon_counts: dict[int, int] = {}
        not_matured = 0
        for start in range(0, len(work.formations), spec.formation_chunk):
            chunk = work.formations[start:start + spec.formation_chunk]
            months = [f.month_index for f in chunk]
            labels = _monthly_labels(con, spec, chunk)
            valid = labels[labels["status"] == "valid"]
            ends = {f.month_index: f.label_end for f in chunk}
            if len(valid):
                _stage_frame(con, "_p4_unused", pd.DataFrame({
                    "month_index": valid["month_index"].to_numpy(np.int64),
                    "security": valid["security"].to_numpy(np.int64),
                    "ret": _floats(valid, "ret"), "available_at": _stamps(valid, "available_at"),
                    "window_end": [ends[m] for m in valid["month_index"].tolist()]}),
                    (("month_index", "BIGINT"), ("security", "BIGINT"), ("ret", "DOUBLE"),
                     ("available_at", "TIMESTAMP"), ("window_end", "DATE")), into="_p4_mret")
            frame = _chunk_frame(con, months, labels, work)
            rows: dict[str, list[Any]] = {"returns": [], "breakpoints": [], "segments": []}
            rf_m = rf.period_returns(np.array([f.formation_date for f in chunk], dtype="datetime64[D]"),
                                     MONTHLY_LABEL_SESSIONS)
            mf_rows = []
            for i, f in enumerate(chunk):
                part = frame[frame["month_index"] == f.month_index]
                result = _monthly_formation(f, part, spec, work, float(rf_m[i]), rf.basis)
                for key in rows:
                    rows[key] += result[key]
                mf_rows += [dict(item, month_index=f.month_index, window_end=f.label_end) for item in result["mf"]]
                if f.label_matured:
                    for row in result["returns"]:
                        if row["factor_id"] == "hml":
                            venue_counts[row["venue_basis"]] = venue_counts.get(row["venue_basis"], 0) + 1
                else:
                    not_matured += 1
            if mf_rows:
                _stage_frame(con, "_p4_unused", pd.DataFrame(mf_rows), (
                    ("month_index", "BIGINT"), ("window_end", "DATE"), ("factor_id", "VARCHAR"), ("value", "DOUBLE"),
                    ("available_at", "TIMESTAMP"), ("venue_basis", "VARCHAR")), into="_p4_mf")
            # Exact h-month factor rows: the same formation portfolios over the R3a h-month label
            # windows (entry + 21h sessions), so an h-month R3b series is spanned like for like.
            for h in spec.horizons_months:
                horizon_chunk = [_horizon_window(f, h, work, spec) for f in chunk]
                sessions = MONTHLY_LABEL_SESSIONS * h
                horizon_frame = _with_returns(frame, _monthly_labels(con, spec, horizon_chunk, sessions))
                rf_h = rf.period_returns(np.array([f.formation_date for f in chunk], dtype="datetime64[D]"), sessions)
                for i, f in enumerate(horizon_chunk):
                    if f.label_matured:
                        rows["returns"] += _monthly_formation(
                            f, horizon_frame[horizon_frame["month_index"] == f.month_index], spec, work,
                            float(rf_h[i]), rf.basis, frequency=horizon_frequency(h), structural=False)["returns"]
                        horizon_counts[h] = horizon_counts.get(h, 0) + 1
            if spec.daily:
                rows["returns"] += _daily_chunk(store, spec, work, chunk, rf)
            exposures = _exposures_chunk(store, spec, work, chunk, rf.basis) if spec.exposures else pd.DataFrame()
            with store.transaction():
                for key in rows:
                    counts[key] += _insert(con, key, rows[key])
                counts["exposures"] += _insert(con, "exposures", exposures)
        diagnostic.update(style_venue_formations=venue_counts, label_windows_not_matured=not_matured,
                          daily=work.diag.get("daily"),
                          horizon_formations={horizon_frequency(h): horizon_counts.get(h, 0)
                                              for h in spec.horizons_months})
        fallback = sum(n for venue, n in venue_counts.items() if venue != VENUE_NYSE_PIT)
        if fallback:
            blockers.append(f"{table.basis}_style_formations_without_pit_nyse_breakpoints:{fallback}")
        if not_matured:
            blockers.append(f"{table.basis}_formations_label_window_not_matured:{not_matured}")
        diagnostic["exposures"] = {row[0]: int(row[1]) for row in con.execute("""
            SELECT status_252d || '|' || status_36m, count(*) FROM research_factor_exposures WHERE run_id=? GROUP BY 1
        """, [spec.run_id]).fetchall()}
        return _finish(store, spec, table.basis, rf.basis, "complete", counts, blockers, diagnostic)
    except Exception as error:
        with contextlib.suppress(Exception):
            con.execute("UPDATE research_factor_runs SET status='failed', diagnostic_json=?, finished_at=? "
                        "WHERE run_id=?", [_canonical({"error": repr(error)[:2000]}), _now(), spec.run_id])
        raise


def _finish(store: ResearchStore, spec: FactorSpec, basis: str, rf_basis: str, status: str, counts: dict[str, int],
            blockers: list[str], diagnostic: dict[str, Any]) -> FactorRunResult:
    con = store.con
    digest, parts = results_digest(con, spec.run_id)
    diagnostic["table_sha256"] = parts
    diagnostic["rows"] = counts
    con.execute("UPDATE research_factor_runs SET status=?, blockers_json=?, diagnostic_json=?, results_sha256=?, "
                "finished_at=? WHERE run_id=?",
                [status, _canonical(blockers), _canonical(diagnostic), digest, _now(), spec.run_id])
    return FactorRunResult(spec.run_id, status, basis, rf_basis, counts, tuple(blockers), digest, diagnostic)


# ---------------------------------------------------------------------------
# Readers
# ---------------------------------------------------------------------------

def load_factor_returns(store: ResearchStore, run_id: str, *, frequency: str = FREQ_MONTHLY,
                        factors: Sequence[str] | None = None) -> pd.DataFrame:
    """Wide factor returns of a complete run: index ``period_date``, one column per factor.

    ``frequency`` is ``daily``, ``monthly`` or ``monthly_h{h}`` (:func:`horizon_frequency`,
    the exact h-month label-window rows; ``period_date`` is the formation date)."""
    con = store.con
    row = con.execute("SELECT status FROM research_factor_runs WHERE run_id=?", [run_id]).fetchone()
    if row is None or row[0] != "complete":
        raise FactorInputError(f"factor run {run_id} is absent or not complete")
    allowed = (FREQ_DAILY, FREQ_MONTHLY, *(horizon_frequency(h) for h in LABEL_HORIZON_MONTHS))
    if frequency not in allowed:
        raise FactorInputError(f"frequency must be one of {allowed}")
    long = con.execute("SELECT period_date, factor_id, value FROM research_factor_returns "
                       "WHERE run_id=? AND frequency=? ORDER BY period_date, factor_id", [run_id, frequency]).df()
    wide = long.pivot(index="period_date", columns="factor_id", values="value")
    wide.index = pd.to_datetime(wide.index)
    if factors is not None:
        missing = sorted(set(factors) - set(wide.columns))
        if missing:
            raise FactorInputError(f"factor run {run_id} has no {frequency} factors {missing}")
        wide = wide[list(factors)]
    return wide.astype(float)


def _month_periods(index: Any, label: str) -> pd.PeriodIndex:
    if isinstance(index, pd.PeriodIndex):
        periods = index.asfreq("M")
    else:
        periods = pd.PeriodIndex(pd.to_datetime(pd.Index(index)).to_period("M"))
    if periods.has_duplicates:
        raise FactorInputError(f"{label}: more than one formation in a calendar month")
    return periods


def compound_factor_windows(monthly: pd.DataFrame, horizon_periods: int, *,
                            factors: Sequence[str] | None = None) -> pd.DataFrame:
    """Monthly factor rows compounded over ``h`` consecutive formations, on the full monthly
    calendar (a ``PeriodIndex``; a month without a formation is a NaN row).

    Row ``p`` covers formations ``p .. p+h-1``: the windows an h-month R3b label formed at
    ``p`` spans (formation -> formation + h). A long-short factor compounds as
    ``prod(1 + f) - 1``; ``mkt_rf`` as ``prod(1 + mkt_vw) - prod(1 + rf)`` (the raw VW
    compounding when the run has no risk-free rate). NaN unless all ``h`` formations are
    present with a value. ``h = 1`` returns the monthly rows themselves on the calendar.
    """
    h = int(horizon_periods)
    if h != horizon_periods or h < 1:
        raise FactorInputError("horizon_periods must be a positive integer")
    names = list(monthly.columns) if factors is None else list(factors)
    missing = sorted(set(names) - set(monthly.columns))
    if missing:
        raise FactorInputError(f"no monthly factors {missing}")
    frame = monthly.copy()
    frame.index = _month_periods(frame.index, "factors")
    grid = pd.period_range(frame.index.min(), frame.index.max(), freq="M")
    frame = frame.reindex(grid)

    def gross(column: str) -> np.ndarray:
        growth = 1.0 + frame[column].to_numpy(dtype=float)
        result = np.full(len(growth), _NAN)
        for p in range(len(growth) - h + 1):
            window = growth[p:p + h]
            if np.isfinite(window).all():
                result[p] = float(np.prod(window))
        return result

    out: dict[str, np.ndarray] = {}
    for name in names:
        if name == "mkt_rf" and "mkt_vw" in frame.columns:
            has_rf = "rf" in frame.columns and bool(frame["rf"].notna().any())
            out[name] = gross("mkt_vw") - (gross("rf") if has_rf else 1.0)
        else:
            out[name] = gross(name) - 1.0
    return pd.DataFrame(out, index=grid)


def span_test_against_run(store: ResearchStore, run_id: str, ls_returns: pd.Series, *,
                          factors: Sequence[str] = EXPOSURE_FACTORS, horizon_periods: int = 1) -> SpanTestResult:
    """:func:`span_test` of an R3b-style series (indexed by formation date or month period;
    ``horizon_periods`` = its label horizon in months) on the run's factors over the same
    windows, both reindexed onto the full monthly calendar so a missing formation is a gap
    in the HAC sums.

    ``h = 1`` uses the monthly rows; ``h > 1`` the run's exact ``monthly_h{h}`` rows (the
    R3a h-month label windows, ``factor_window_basis='exact_label_window'``) when it has
    them, else the monthly rows compounded over h formations (:func:`compound_factor_windows`,
    ``'compounded_21_session_windows'``; ``significance_claimable`` is False at h >= 6)."""
    h = int(horizon_periods)
    if h != horizon_periods or h < 1:
        raise FactorInputError("horizon_periods must be a positive integer")
    frequency = horizon_frequency(h)
    has_exact = h == 1 or (h in LABEL_HORIZON_MONTHS and store.con.execute(
        "SELECT count(*) FROM research_factor_returns WHERE run_id=? AND frequency=?",
        [run_id, frequency]).fetchone()[0] > 0)
    if has_exact:
        windows = load_factor_returns(store, run_id, frequency=frequency, factors=factors)
        windows.index = _month_periods(windows.index, "factors")
        basis = WINDOW_EXACT
    else:
        windows = compound_factor_windows(load_factor_returns(store, run_id), h, factors=factors)
        basis = WINDOW_COMPOUNDED
    series = pd.Series(ls_returns, dtype=float).copy()
    series.index = _month_periods(series.index, "ls_returns")
    grid = pd.period_range(min(series.index.min(), windows.index.min()),
                           max(series.index.max(), windows.index.max()), freq="M")
    return span_test(series.reindex(grid), windows.reindex(grid), horizon_periods=h, factor_window_basis=basis)


# ---------------------------------------------------------------------------
# Spanning of every evaluation cell (tier-1 v2 node 4.1; reported, never gating)
# ---------------------------------------------------------------------------

#: Factor models every R3b cell's long-short is spanned against (``BasisInputs.factors`` keys) and the factor
#: columns each needs: the atx P4 CAPM and FF5 + UMD analogs, and the external French FF5 + UMD and HXZ q5
#: benchmark files (X.3). A model the caller did not supply is reported as NULL, never approximated.
SPAN_MODELS: dict[str, tuple[str, ...]] = {
    "capm_atx": ("mkt_rf",),
    "ff6_atx": ("mkt_rf", "smb", "hml", "rmw", "cma", "umd"),
    "capm_french": ("mkt_rf",),
    "ff6_french": ("mkt_rf", "smb", "hml", "rmw", "cma", "umd"),
    "q5": ("r_mkt", "r_me", "r_ia", "r_roe", "r_eg"),
}
#: Market excess-return columns and their risk-free column: over h months the excess return compounds as
#: ``prod(1 + excess + rf) - prod(1 + rf)`` when the model frame carries the risk-free column.
SPAN_EXCESS_MARKETS: dict[str, str] = {"mkt_rf": "rf", "r_mkt": "r_f"}
#: ``BasisInputs.factors`` row convention: the row of formation ``month_index`` m holds each factor's return over
#: the calendar month after m's month end (the window of m's one-month label).
SPAN_ROW_RULE = "row m = next monthly factor return after formation m; external calendar months proxy 21-session labels"
WINDOW_FORMATION_COMPOUNDED = "compounded_monthly_formation_rows"
WINDOW_BENCHMARK_MONTH = "external_calendar_month_proxy_for_21_session_labels"
_BENCH_FRENCH = "bench_french_ff5_umd_monthly"
_BENCH_Q5 = "bench_q5"


def compound_formation_rows(values: np.ndarray, horizon_periods: int, rf: np.ndarray | None = None) -> np.ndarray:
    """Per formation row m: the factor return over the h monthly rows ``m .. m+h-1`` (NaN unless all present).

    ``prod(1 + f) - 1``; with ``rf`` (a market *excess* column's risk-free rate) ``prod(1 + f + rf) -
    prod(1 + rf)``. ``h = 1`` returns the rows unchanged.
    """
    h = int(horizon_periods)
    if h != horizon_periods or h < 1:
        raise FactorInputError("horizon_periods must be a positive integer")
    values = np.asarray(values, dtype=float)
    if values.ndim != 1 or (rf is not None and np.asarray(rf).shape != values.shape):
        raise FactorInputError("formation factors and risk-free rows must be aligned one-dimensional arrays")
    if h == 1:
        return values.copy()
    months = len(values)
    out = np.full(months, _NAN)
    total = values if rf is None else values + np.asarray(rf, dtype=float)
    for m in range(months - h + 1):
        window = total[m:m + h]
        if not np.isfinite(window).all():
            continue
        gross = float(np.prod(1.0 + window))
        if rf is None:
            out[m] = gross - 1.0
        else:
            free = np.asarray(rf, dtype=float)[m:m + h]
            out[m] = gross - float(np.prod(1.0 + free)) if np.isfinite(free).all() else _NAN
    return out


def span_cell(ls_returns: np.ndarray, factors: Mapping[str, np.ndarray], *, horizon_periods: int,
              model: str, monthly_window_basis: str = WINDOW_EXACT) -> dict[str, Any]:
    """Spanning regression of one R3b cell's per-formation long-short on one factor model (node 4.1).

    ``ls_returns``: the cell's h-month long-short per formation (NaN outside its tested sample);
    ``factors``: the model's monthly factor columns per formation row (:data:`SPAN_ROW_RULE`), plus the
    risk-free column of an excess market column when available. For ``h > 1`` the factor rows are
    compounded over the h formations of each label window (:func:`compound_formation_rows`,
    ``window_basis='compounded_monthly_formation_rows'``, no significance claim at ``h >= 6`` as for
    :data:`WINDOW_COMPOUNDED`). Returns alpha, its Newey-West t (lags ``max(h-1, floor(4 (T/100)^(2/9)))``),
    its EWC robust p, R^2 and the periods used; external calendar-month factors are explicitly a proxy
    for 21-session labels (``monthly_window_basis``), never a claimable exact-window test. NULLs when
    the regression is not estimable (too few
    complete periods, a collinear design).
    """
    if model not in SPAN_MODELS:
        raise FactorInputError(f"unknown span model {model!r}; known {sorted(SPAN_MODELS)}")
    if monthly_window_basis not in (WINDOW_EXACT, WINDOW_BENCHMARK_MONTH):
        raise FactorInputError(f"unknown monthly span window basis {monthly_window_basis!r}")
    h = int(horizon_periods)
    if h != horizon_periods or h < 1:
        raise FactorInputError("horizon_periods must be a positive integer")
    names = SPAN_MODELS[model]
    missing = [name for name in names if name not in factors]
    if missing:
        raise FactorInputError(f"span model {model} lacks factor columns {missing}")
    if any(np.asarray(factors[name]).shape != np.asarray(ls_returns).shape for name in names):
        raise FactorInputError("span factors must align with the long-short formation grid")
    columns = {}
    for name in names:
        rf = factors.get(SPAN_EXCESS_MARKETS[name]) if name in SPAN_EXCESS_MARKETS else None
        columns[name] = compound_formation_rows(np.asarray(factors[name], dtype=float), h, rf)
    basis = monthly_window_basis if h == 1 else WINDOW_FORMATION_COMPOUNDED
    if h > 1 and monthly_window_basis == WINDOW_BENCHMARK_MONTH:
        basis = f"{WINDOW_BENCHMARK_MONTH};{WINDOW_FORMATION_COMPOUNDED}"
    result: dict[str, Any] = {"alpha": None, "alpha_nw_t": None, "alpha_robust_p": None, "r2": None, "n": 0,
                              "window_basis": basis,
                              "claimable": monthly_window_basis != WINDOW_BENCHMARK_MONTH
                              and h <= COMPOUNDED_CLAIM_MAX_MONTHS}
    y = pd.Series(np.asarray(ls_returns, dtype=float))
    frame = pd.DataFrame(columns, index=y.index)
    try:
        test = span_test(y, frame, horizon_periods=h, factor_window_basis=WINDOW_CALLER)
    except FactorInputError:
        return result
    inference = test.alpha_inference
    result.update({"alpha": _finite(test.alpha), "alpha_nw_t": _finite(inference.nw_t),
                   "alpha_robust_p": _finite(inference.robust_p_value), "r2": _finite(test.r2), "n": test.n_obs})
    return result


def _finite(value: Any) -> float | None:
    number = float(value)
    return number if math.isfinite(number) else None


def _month_after(day: dt.date) -> tuple[int, int]:
    return (day.year + 1, 1) if day.month == 12 else (day.year, day.month + 1)


def _formation_rows(calendar: pd.DataFrame, monthly: Mapping[tuple[int, int], Mapping[str, float]],
                    names: Sequence[str]) -> pd.DataFrame:
    """One row per calendar formation: the factor returns of the calendar month after its month end."""
    rows = []
    for index, start in zip(calendar["month_index"], pd.to_datetime(calendar["month_start"]), strict=True):
        values = monthly.get(_month_after(start.date()), {})
        rows.append({"month_index": int(index), **{name: values.get(name, _NAN) for name in names}})
    return pd.DataFrame(rows, columns=["month_index", *names])


def benchmark_span_factors(calendar: pd.DataFrame, bench_snapshot: str, *,
                           root: Path | str | None = None) -> dict[str, pd.DataFrame]:
    """The external span models of ``BasisInputs.factors`` from a benchmark lake snapshot (X.3 files).

    ``capm_french`` / ``ff6_french`` from the French FF5 + UMD monthly file (with ``rf``) and ``q5`` from
    the HXZ q5 file (``r_mkt`` is the market *excess* return, with ``r_f``), both in decimals, one row per
    calendar formation (:data:`SPAN_ROW_RULE`; ``calendar`` is the R3b ``BasisInputs.calendar``). A dataset
    missing from the snapshot leaves its models out (reported NULL by the engine).
    """
    from . import research_lake as lake

    out: dict[str, pd.DataFrame] = {}
    con = lake.connect_bounded(None, root=root, memory_limit="64MB", threads=1)
    try:
        for dataset, models in ((_BENCH_FRENCH, ("capm_french", "ff6_french")), (_BENCH_Q5, ("q5",))):
            try:
                files = lake.lake_files(bench_snapshot, dataset, root=root)
            except (lake.LakeError, FileNotFoundError):
                continue
            wanted = ("mkt_rf", "smb", "hml", "rmw", "cma", "umd", "rf") if dataset == _BENCH_FRENCH else \
                ("r_mkt", "r_me", "r_ia", "r_roe", "r_eg", "r_f")
            listing = "[" + ", ".join(lake.sql_text(path) for path in files) + "]"
            records = con.execute(f"SELECT month_end, {', '.join(wanted)} FROM read_parquet({listing})").fetchall()
            monthly = {(row[0].year, row[0].month): {name: (_NAN if value is None else float(value))
                                                     for name, value in zip(wanted, row[1:], strict=True)}
                       for row in records}
            frame = _formation_rows(calendar, monthly, wanted)
            for model in models:
                extra = [SPAN_EXCESS_MARKETS[n] for n in SPAN_MODELS[model] if n in SPAN_EXCESS_MARKETS]
                out[model] = frame[["month_index", *SPAN_MODELS[model], *extra]].copy()
                out[model].attrs["monthly_window_basis"] = WINDOW_BENCHMARK_MONTH
    finally:
        con.close()
    return out


def atx_span_factors(store: ResearchStore, run_id: str, calendar: pd.DataFrame) -> dict[str, pd.DataFrame]:
    """The atx P4 span models (``capm_atx``, ``ff6_atx``) of ``BasisInputs.factors`` from a complete factor run.

    Monthly P4 rows are dated at their formation (``period_date``), so row m is the factor run's row of
    formation m (matched on the formation date of ``calendar``). ``mkt_rf`` compounds with the run's ``rf``.
    """
    monthly = load_factor_returns(store, run_id)
    names = [name for name in (*SPAN_MODELS["ff6_atx"], "rf") if name in monthly.columns]
    by_date = {ts.date(): row for ts, row in monthly.iterrows()}
    rows = []
    for index, formed in zip(calendar["month_index"], pd.to_datetime(calendar["formation_date"]), strict=True):
        row = by_date.get(formed.date()) if pd.notna(formed) else None
        rows.append({"month_index": int(index),
                     **{name: (_NAN if row is None else float(row[name])) for name in names}})
    frame = pd.DataFrame(rows, columns=["month_index", *names])
    out: dict[str, pd.DataFrame] = {}
    for model in ("capm_atx", "ff6_atx"):
        if all(name in names for name in SPAN_MODELS[model]):
            extra = ["rf"] if "rf" in names else []
            out[model] = frame[["month_index", *SPAN_MODELS[model], *extra]].copy()
    return out


def benchmark_original_paper_t(bench_snapshot: str, *, root: Path | str | None = None) -> dict[str, float]:
    """Original-paper t statistics from the pinned X.3 OSAP SignalDoc snapshot (no network access)."""
    from . import research_lake as lake

    try:
        files = lake.lake_files(bench_snapshot, "bench_osap_signaldoc", root=root)
    except (lake.LakeError, FileNotFoundError):
        return {}
    con = lake.connect_bounded(None, root=root, memory_limit="64MB", threads=1)
    try:
        listing = "[" + ", ".join(lake.sql_text(path) for path in files) + "]"
        rows = con.execute(f"SELECT acronym, t_stat FROM read_parquet({listing})").fetchall()
        return {str(name): float(value) for name, value in rows if value is not None and math.isfinite(float(value))}
    finally:
        con.close()


# ---------------------------------------------------------------------------
# CLI: python -m atx_db.research.factor_returns {fetch-rf,build}
# ---------------------------------------------------------------------------

def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m atx_db.research.factor_returns")
    commands = parser.add_subparsers(dest="command", required=True)
    fetch = commands.add_parser("fetch-rf", help="fetch FRED DTB3 into the P4 cache (ruling RX10)")
    fetch.add_argument("--cache-dir")
    build = commands.add_parser("build", help="build one factor run in the research store")
    build.add_argument("--run-id", required=True)
    build.add_argument("--feature-version", required=True)
    build.add_argument("--label-cutoff", required=True, help="ISO timestamp, UTC")
    build.add_argument("--industry-version", action="append", default=[])
    build.add_argument("--research-db")
    build.add_argument("--warehouse")
    build.add_argument("--rf-path")
    build.add_argument("--no-rf", action="store_true")
    build.add_argument("--no-verify", action="store_true")
    build.add_argument("--replace", action="store_true")
    build.add_argument("--memory-limit", default="384MB")
    build.add_argument("--threads", type=int, default=2)
    args = parser.parse_args(argv)
    if args.command == "fetch-rf":
        path = fetch_fred_dtb3(args.cache_dir)
        print(json.dumps(load_risk_free(path).manifest(), sort_keys=True))
        return 0
    cutoff = dt.datetime.fromisoformat(args.label_cutoff)
    if cutoff.tzinfo is None:
        cutoff = cutoff.replace(tzinfo=dt.UTC)
    rf = RiskFreeSeries.none() if args.no_rf else load_risk_free(args.rf_path)
    spec = FactorSpec(run_id=args.run_id, feature_version=args.feature_version, label_cutoff=cutoff,
                      industry_versions=tuple(args.industry_version), verify_panels=not args.no_verify)
    with ResearchStore(args.research_db, warehouse_path=args.warehouse, memory_limit=args.memory_limit,
                       threads=args.threads) as store:
        result = build_factor_returns(store, spec, risk_free=rf, replace_run=args.replace)
    print(json.dumps({"run_id": result.run_id, "status": result.status, "rows": result.rows,
                      "rf_basis": result.rf_basis, "blockers": list(result.blockers),
                      "results_sha256": result.results_sha256}, sort_keys=True))
    return 0


__all__ = [
    "EXPOSURE_FACTORS",
    "FACTOR_CONSTRUCTS",
    "FACTOR_VERSION",
    "LABEL_HORIZON_MONTHS",
    "RF_DTB3",
    "RF_NONE",
    "SORTS",
    "SPAN_EXCESS_MARKETS",
    "SPAN_MODELS",
    "SPAN_ROW_RULE",
    "STYLE_FACTORS",
    "WINDOW_COMPOUNDED",
    "WINDOW_EXACT",
    "WINDOW_FORMATION_COMPOUNDED",
    "FactorInputError",
    "FactorLookaheadError",
    "FactorRunResult",
    "FactorSpec",
    "RiskFreeSeries",
    "SpanTestResult",
    "assign_groups",
    "atx_span_factors",
    "benchmark_original_paper_t",
    "benchmark_span_factors",
    "build_factor_returns",
    "compound_factor_windows",
    "compound_formation_rows",
    "default_rf_path",
    "ensure_factor_schema",
    "fetch_fred_dtb3",
    "grouped_ols",
    "horizon_frequency",
    "load_factor_returns",
    "load_risk_free",
    "parse_fred_series",
    "reference_names",
    "results_digest",
    "span_cell",
    "span_test",
    "span_test_against_run",
    "validate_factor_spec",
]


if __name__ == "__main__":
    sys.exit(main())
