"""S9a: derived point-in-time financial ratios (Compustat/FactSet calculated items).

This module materializes ``fundamental_ratios``, a long-format fact table with one
row per (security, period, ratio_code). Ratios are computed from two already-loaded,
already-bitemporal warehouse surfaces:

* ``fundamental_ttm_points``        -- trailing-twelve-month *flow* metrics (revenue,
                                       net income, operating cash flow, capex, ...).
                                       TTM is the quant-standard, fiscal-calendar-robust
                                       flow basis and avoids the Q-vs-YTD ambiguity of
                                       raw statement flows.
* ``fundamental_statement_points``  -- *instant* balance metrics at the period close
                                       (assets, liabilities, equity, shares).

Point-in-time discipline: every ratio carries its **own** ``available_at`` equal to the
maximum availability of the specific inputs it consumes, so an as-of query returns a
ratio only once every input it depends on was knowable. ``as_of_date`` is the period
close (TTM window end). No network is touched — ratios are a pure transform of warehouse
tables, so the dataset is fully deterministic and test-safe.

The ratio math lives in :func:`compute_ratio_rows`, a pure DataFrame->DataFrame transform
that is unit-tested independently of DuckDB. The :class:`FundamentalRatiosDataset` /
:func:`refresh_fundamental_ratios` pair pivots the inputs into the wide frame the transform
expects, then writes the result.

v1 stores only the latest-revision vintage of the inputs (one ratio per period). Storing a
ratio row per restatement vintage is a planned, non-breaking refinement (the bitemporal
columns are already present).
"""
from __future__ import annotations

import datetime as dt
import hashlib
from dataclasses import dataclass
from typing import Any, Callable

import pandas as pd

from .asof import fundamental_ratios_asof  # noqa: F401  (re-exported for callers)
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, quality_check


SOURCE_NAME = "Derived point-in-time financial ratios"
DEFAULT_SOURCE = "derived_fundamental_ratios_v1"
DEFAULT_BASIS = "ttm"

# Raw wide-frame input key -> canonical_metric in the source tables.
TTM_INPUTS = {
    "rev": "revenue",
    "ni": "net_income",
    "oi": "operating_income",
    "ocf": "operating_cash_flow",
    "capex": "capital_expenditures",
    "div": "dividends_paid",
    "repurch": "share_repurchases",
}
BALANCE_INPUTS = {
    "assets": "assets",
    "liabilities": "liabilities",
    "equity": "stockholders_equity",
    "shares": "shares_outstanding",
}

RATIO_COLUMNS = [
    "ratio_id", "source", "upstream_source", "security_id", "symbol", "cik",
    "ratio_code", "ratio_category", "ratio_kind", "basis", "unit",
    "period_start", "period_end", "fiscal_year", "fiscal_period",
    "value", "numerator_code", "numerator_value", "denominator_code",
    "denominator_value", "is_meaningful", "is_latest_revision",
    "as_of_date", "available_at", "input_codes_json", "run_id",
]


@dataclass(frozen=True)
class RatioDef:
    code: str
    category: str          # profitability | leverage | cash_flow | payout | per_share
    kind: str              # ratio | level | per_share
    unit: str              # ratio | currency | currency_per_share
    numerator_code: str
    denominator_code: str
    inputs: tuple[str, ...]            # raw input keys that gate availability
    operands: Callable[[dict], tuple]  # (numerator_value, denominator_value)
    require_positive_denominator: bool = False


def _abs(x: float) -> float:
    return abs(float(x))


def _z(x: Any) -> float:
    """Coalesce a missing/NaN optional input to 0.0 (e.g. unreported inventory)."""
    try:
        return 0.0 if pd.isna(x) else float(x)
    except (TypeError, ValueError):
        return 0.0 if x is None else float(x)


RATIO_DEFS: tuple[RatioDef, ...] = (
    # --- profitability -----------------------------------------------------
    RatioDef("net_profit_margin", "profitability", "ratio", "ratio",
             "net_income", "revenue", ("ni", "rev"),
             lambda r: (r["ni"], r["rev"])),
    RatioDef("operating_margin", "profitability", "ratio", "ratio",
             "operating_income", "revenue", ("oi", "rev"),
             lambda r: (r["oi"], r["rev"])),
    RatioDef("return_on_assets", "profitability", "ratio", "ratio",
             "net_income", "assets", ("ni", "assets"),
             lambda r: (r["ni"], r["assets"])),
    RatioDef("return_on_equity", "profitability", "ratio", "ratio",
             "net_income", "stockholders_equity", ("ni", "equity"),
             lambda r: (r["ni"], r["equity"]), require_positive_denominator=True),
    # --- leverage ----------------------------------------------------------
    RatioDef("assets_to_equity", "leverage", "ratio", "ratio",
             "assets", "stockholders_equity", ("assets", "equity"),
             lambda r: (r["assets"], r["equity"]), require_positive_denominator=True),
    RatioDef("liabilities_to_assets", "leverage", "ratio", "ratio",
             "liabilities", "assets", ("liabilities", "assets"),
             lambda r: (r["liabilities"], r["assets"])),
    RatioDef("liabilities_to_equity", "leverage", "ratio", "ratio",
             "liabilities", "stockholders_equity", ("liabilities", "equity"),
             lambda r: (r["liabilities"], r["equity"]), require_positive_denominator=True),
    # --- cash flow ---------------------------------------------------------
    RatioDef("free_cash_flow", "cash_flow", "level", "currency",
             "operating_cash_flow", "capital_expenditures", ("ocf", "capex"),
             lambda r: (r["ocf"], r["capex"])),
    RatioDef("fcf_margin", "cash_flow", "ratio", "ratio",
             "free_cash_flow", "revenue", ("ocf", "capex", "rev"),
             lambda r: (r["ocf"] + r["capex"], r["rev"])),
    RatioDef("operating_cash_flow_to_net_income", "cash_flow", "ratio", "ratio",
             "operating_cash_flow", "net_income", ("ocf", "ni"),
             lambda r: (r["ocf"], r["ni"]), require_positive_denominator=True),
    RatioDef("capex_to_revenue", "cash_flow", "ratio", "ratio",
             "abs_capital_expenditures", "revenue", ("capex", "rev"),
             lambda r: (_abs(r["capex"]), r["rev"])),
    # --- payout ------------------------------------------------------------
    RatioDef("dividend_payout_ratio", "payout", "ratio", "ratio",
             "abs_dividends_paid", "net_income", ("div", "ni"),
             lambda r: (_abs(r["div"]), r["ni"]), require_positive_denominator=True),
    RatioDef("buyback_to_net_income", "payout", "ratio", "ratio",
             "abs_share_repurchases", "net_income", ("repurch", "ni"),
             lambda r: (_abs(r["repurch"]), r["ni"]), require_positive_denominator=True),
    RatioDef("total_payout_ratio", "payout", "ratio", "ratio",
             "abs_dividends_plus_repurchases", "net_income", ("div", "repurch", "ni"),
             lambda r: (_abs(r["div"]) + _abs(r["repurch"]), r["ni"]),
             require_positive_denominator=True),
    # --- per share ---------------------------------------------------------
    RatioDef("book_value_per_share", "per_share", "per_share", "currency_per_share",
             "stockholders_equity", "shares_outstanding", ("equity", "shares"),
             lambda r: (r["equity"], r["shares"])),
    # --- efficiency / activity (S9b) --------------------------------------
    RatioDef("asset_turnover", "efficiency", "ratio", "ratio",
             "revenue", "assets", ("rev", "assets"),
             lambda r: (r["rev"], r["assets"])),
    RatioDef("equity_turnover", "efficiency", "ratio", "ratio",
             "revenue", "stockholders_equity", ("rev", "equity"),
             lambda r: (r["rev"], r["equity"]), require_positive_denominator=True),
    # --- additional profitability / cash-flow coverage (S9b) --------------
    RatioDef("operating_return_on_assets", "profitability", "ratio", "ratio",
             "operating_income", "assets", ("oi", "assets"),
             lambda r: (r["oi"], r["assets"])),
    RatioDef("operating_cash_flow_margin", "cash_flow", "ratio", "ratio",
             "operating_cash_flow", "revenue", ("ocf", "rev"),
             lambda r: (r["ocf"], r["rev"])),
    RatioDef("operating_cash_flow_to_assets", "cash_flow", "ratio", "ratio",
             "operating_cash_flow", "assets", ("ocf", "assets"),
             lambda r: (r["ocf"], r["assets"])),
    RatioDef("operating_cash_flow_to_liabilities", "cash_flow", "ratio", "ratio",
             "operating_cash_flow", "liabilities", ("ocf", "liabilities"),
             lambda r: (r["ocf"], r["liabilities"]), require_positive_denominator=True),
    RatioDef("capex_to_operating_cash_flow", "cash_flow", "ratio", "ratio",
             "abs_capital_expenditures", "operating_cash_flow", ("capex", "ocf"),
             lambda r: (_abs(r["capex"]), r["ocf"]), require_positive_denominator=True),
    # --- payout / reinvestment (S9b) --------------------------------------
    RatioDef("retention_ratio", "payout", "ratio", "ratio",
             "net_income_minus_dividends", "net_income", ("ni", "div"),
             lambda r: (r["ni"] - _abs(r["div"]), r["ni"]), require_positive_denominator=True),
    # --- year-over-year growth (S9c) --------------------------------------
    # kind='growth': value = (current - prior) / abs(prior); the prior-year value is
    # paired into the wide frame by _attach_prior_year. is_meaningful is false off a
    # non-positive base (sign of a % change is ambiguous when the prior is negative).
    RatioDef("revenue_growth_yoy", "growth", "growth", "ratio",
             "revenue", "revenue_prior_year", ("rev", "rev_prior"),
             lambda r: (r["rev"], r["rev_prior"]), require_positive_denominator=True),
    RatioDef("net_income_growth_yoy", "growth", "growth", "ratio",
             "net_income", "net_income_prior_year", ("ni", "ni_prior"),
             lambda r: (r["ni"], r["ni_prior"]), require_positive_denominator=True),
    RatioDef("operating_income_growth_yoy", "growth", "growth", "ratio",
             "operating_income", "operating_income_prior_year", ("oi", "oi_prior"),
             lambda r: (r["oi"], r["oi_prior"]), require_positive_denominator=True),
    RatioDef("operating_cash_flow_growth_yoy", "growth", "growth", "ratio",
             "operating_cash_flow", "operating_cash_flow_prior_year", ("ocf", "ocf_prior"),
             lambda r: (r["ocf"], r["ocf_prior"]), require_positive_denominator=True),
    RatioDef("assets_growth_yoy", "growth", "growth", "ratio",
             "assets", "assets_prior_year", ("assets", "assets_prior"),
             lambda r: (r["assets"], r["assets_prior"]), require_positive_denominator=True),
    RatioDef("equity_growth_yoy", "growth", "growth", "ratio",
             "stockholders_equity", "stockholders_equity_prior_year", ("equity", "equity_prior"),
             lambda r: (r["equity"], r["equity_prior"]), require_positive_denominator=True),
    # --- average-balance returns (S9d) ------------------------------------
    # Denominator is the mean of the ending and prior-year balance, matching the
    # Compustat/FactSet convention for return-on-capital ratios.
    RatioDef("average_return_on_assets", "profitability", "ratio", "ratio",
             "net_income", "average_assets", ("ni", "assets", "assets_prior"),
             lambda r: (r["ni"], (r["assets"] + r["assets_prior"]) / 2), require_positive_denominator=True),
    RatioDef("average_return_on_equity", "profitability", "ratio", "ratio",
             "net_income", "average_stockholders_equity", ("ni", "equity", "equity_prior"),
             lambda r: (r["ni"], (r["equity"] + r["equity_prior"]) / 2), require_positive_denominator=True),
    RatioDef("operating_cash_flow_to_average_assets", "cash_flow", "ratio", "ratio",
             "operating_cash_flow", "average_assets", ("ocf", "assets", "assets_prior"),
             lambda r: (r["ocf"], (r["assets"] + r["assets_prior"]) / 2), require_positive_denominator=True),
    # --- liquidity / working capital (S10a; consolidated inline-XBRL instants) ---
    RatioDef("current_ratio", "liquidity", "ratio", "ratio",
             "current_assets", "current_liabilities", ("current_assets", "current_liabilities"),
             lambda r: (r["current_assets"], r["current_liabilities"]), require_positive_denominator=True),
    RatioDef("quick_ratio", "liquidity", "ratio", "ratio",
             "current_assets_minus_inventory", "current_liabilities", ("current_assets", "current_liabilities"),
             lambda r: (r["current_assets"] - _z(r.get("inventory")), r["current_liabilities"]),
             require_positive_denominator=True),
    RatioDef("cash_ratio", "liquidity", "ratio", "ratio",
             "cash_and_equivalents", "current_liabilities", ("cash_and_equivalents", "current_liabilities"),
             lambda r: (r["cash_and_equivalents"], r["current_liabilities"]), require_positive_denominator=True),
    RatioDef("working_capital", "liquidity", "difference", "currency",
             "current_assets", "current_liabilities", ("current_assets", "current_liabilities"),
             lambda r: (r["current_assets"], r["current_liabilities"])),
    RatioDef("working_capital_to_assets", "liquidity", "ratio", "ratio",
             "working_capital", "assets", ("current_assets", "current_liabilities", "assets"),
             lambda r: (r["current_assets"] - r["current_liabilities"], r["assets"]),
             require_positive_denominator=True),
    # --- debt / solvency (S10b; long-term debt from consolidated XBRL instants) ---
    RatioDef("long_term_debt_to_equity", "leverage", "ratio", "ratio",
             "long_term_debt", "stockholders_equity", ("long_term_debt", "equity"),
             lambda r: (r["long_term_debt"], r["equity"]), require_positive_denominator=True),
    RatioDef("long_term_debt_to_assets", "leverage", "ratio", "ratio",
             "long_term_debt", "assets", ("long_term_debt", "assets"),
             lambda r: (r["long_term_debt"], r["assets"]), require_positive_denominator=True),
    RatioDef("net_debt", "leverage", "difference", "currency",
             "long_term_debt", "cash_and_equivalents", ("long_term_debt", "cash_and_equivalents"),
             lambda r: (r["long_term_debt"], r["cash_and_equivalents"])),
    RatioDef("net_debt_to_assets", "leverage", "ratio", "ratio",
             "net_debt", "assets", ("long_term_debt", "cash_and_equivalents", "assets"),
             lambda r: (r["long_term_debt"] - r["cash_and_equivalents"], r["assets"]),
             require_positive_denominator=True),
)

# Instant (balance) metrics sourced from the consolidated inline-XBRL extraction
# (fundamental_xbrl_metric), pivoted into the wide frame alongside statement-point balances.
XBRL_BALANCE_INPUTS = {
    "current_assets": "current_assets",
    "current_liabilities": "current_liabilities",
    "cash_and_equivalents": "cash_and_equivalents",
    "inventory": "inventory",
    "long_term_debt": "long_term_debt",
}

# Metrics for which a prior-year value is paired in (for YoY growth and
# average-balance ratios).
GROWTH_PRIOR_KEYS = ("rev", "ni", "oi", "ocf", "assets", "equity")


@dataclass(frozen=True)
class FundamentalRatiosOptions:
    source: str = DEFAULT_SOURCE
    basis: str = DEFAULT_BASIS
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


def _present(value: Any) -> bool:
    try:
        return not pd.isna(value)
    except (TypeError, ValueError):
        return value is not None


def _ratio_id(source: str, security_id: str, ratio_code: str, basis: str,
              period_end: Any, available_at: Any) -> str:
    payload = "|".join(str(p) for p in (source, security_id, ratio_code, basis, period_end, available_at))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def compute_ratio_rows(
    inputs: pd.DataFrame,
    *,
    source: str = DEFAULT_SOURCE,
    basis: str = DEFAULT_BASIS,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: wide per-(security, period) input frame -> long ratio rows.

    Each input column ``<key>`` carries a sibling ``<key>_av`` availability timestamp.
    A ratio is emitted only when every input it depends on (value AND availability) is
    present and the denominator is usable; its ``available_at`` is the max input
    availability and its ``as_of_date`` is the period close.
    """
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=RATIO_COLUMNS)

    records: list[dict] = []
    for rec in inputs.to_dict("records"):
        period_end = rec.get("period_end")
        for d in RATIO_DEFS:
            if not all(_present(rec.get(k)) for k in d.inputs):
                continue
            avs = [rec.get(f"{k}_av") for k in d.inputs]
            if not all(_present(a) for a in avs):
                continue
            num, den = d.operands(rec)
            if not _present(num) or not _present(den):
                continue
            num = float(num)
            den = float(den)
            if d.kind == "level":
                value = num + den
                is_meaningful = True
            elif d.kind == "difference":
                value = num - den
                is_meaningful = True
            elif d.kind == "growth":
                if den == 0:
                    continue
                value = (num - den) / abs(den)
                is_meaningful = (not d.require_positive_denominator) or den > 0
            else:
                if den == 0:
                    continue
                value = num / den
                is_meaningful = (not d.require_positive_denominator) or den > 0
            available_at = max(avs)
            records.append(
                {
                    "ratio_id": _ratio_id(source, rec.get("security_id"), d.code, basis, period_end, available_at),
                    "source": source,
                    "upstream_source": rec.get("upstream_source"),
                    "security_id": rec.get("security_id"),
                    "symbol": rec.get("symbol"),
                    "cik": rec.get("cik"),
                    "ratio_code": d.code,
                    "ratio_category": d.category,
                    "ratio_kind": d.kind,
                    "basis": basis,
                    "unit": d.unit,
                    "period_start": rec.get("period_start"),
                    "period_end": period_end,
                    "fiscal_year": rec.get("fiscal_year"),
                    "fiscal_period": rec.get("fiscal_period"),
                    "value": value,
                    "numerator_code": d.numerator_code,
                    "numerator_value": num,
                    "denominator_code": d.denominator_code,
                    "denominator_value": den,
                    "is_meaningful": is_meaningful,
                    "is_latest_revision": True,
                    "as_of_date": period_end,
                    "available_at": available_at,
                    "input_codes_json": json_dumps(list(d.inputs)),
                    "run_id": run_id,
                }
            )

    if not records:
        return pd.DataFrame(columns=RATIO_COLUMNS)
    return pd.DataFrame(records, columns=RATIO_COLUMNS)


def _as_date(value: Any) -> dt.date | None:
    if value is None or (not isinstance(value, dt.date) and pd.isna(value)):
        return None
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    return pd.Timestamp(value).date()


def _attach_prior_year(wide: pd.DataFrame) -> pd.DataFrame:
    """Pair each (security, period_end) row with its ~1-year-earlier row.

    For each `GROWTH_PRIOR_KEYS` metric, copies the prior period's value and its
    availability into `<key>_prior` / `<key>_prior_av`. The prior row is the one
    whose period_end falls 350-380 days before the current period_end (closest to
    365); rows with no such match get NA, so dependent growth ratios are skipped.
    """
    out = wide.copy()
    keys = [k for k in GROWTH_PRIOR_KEYS if k in out.columns and f"{k}_av" in out.columns]
    for key in keys:
        out[f"{key}_prior"] = pd.NA
        out[f"{key}_prior_av"] = pd.NaT
    if out.empty or "period_end" not in out.columns or "security_id" not in out.columns:
        return out

    ends = {idx: _as_date(pe) for idx, pe in out["period_end"].items()}
    for _, grp in out.groupby("security_id", sort=False):
        idxs = list(grp.index)
        for i in idxs:
            cur = ends.get(i)
            if cur is None:
                continue
            best, best_diff = None, None
            for j in idxs:
                prev = ends.get(j)
                if prev is None or prev >= cur:
                    continue
                gap = (cur - prev).days
                if 350 <= gap <= 380:
                    diff = abs(gap - 365)
                    if best_diff is None or diff < best_diff:
                        best, best_diff = j, diff
            if best is not None:
                for key in keys:
                    out.at[i, f"{key}_prior"] = out.at[best, key]
                    out.at[i, f"{key}_prior_av"] = out.at[best, f"{key}_av"]
    return out


def _pivot_case(prefix: str, value_col: str, metric_map: dict[str, str]) -> str:
    parts = []
    for key, metric in metric_map.items():
        parts.append(
            f"max(CASE WHEN {prefix}.canonical_metric = '{metric}' THEN {prefix}.{value_col} END) AS {key}"
        )
        parts.append(
            f"max(CASE WHEN {prefix}.canonical_metric = '{metric}' THEN {prefix}.available_at END) AS {key}_av"
        )
    return ",\n            ".join(parts)


def load_ratio_inputs(store: DuckDBStore, options: FundamentalRatiosOptions) -> pd.DataFrame:
    """Pivot the latest-revision TTM flows + instant balances into the wide input frame."""
    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    registered = False
    sym_join_t = ""
    sym_join_b = ""
    if symbols:
        store.con.register(
            "ratio_symbol_filter",
            pd.DataFrame({"symbol": sorted({str(s).strip().upper() for s in symbols})}),
        )
        registered = True
        sym_join_t = "JOIN ratio_symbol_filter rsf ON rsf.symbol = t.symbol"
        sym_join_b = "JOIN ratio_symbol_filter rsf ON rsf.symbol = s.symbol"
        sym_join_x = "JOIN ratio_symbol_filter rsf ON rsf.symbol = x.symbol"
    else:
        sym_join_x = ""

    sql = f"""
        WITH ttm AS (
            SELECT
                t.security_id,
                any_value(t.symbol) AS symbol,
                any_value(t.cik) AS cik,
                any_value(t.source) AS upstream_source,
                t.ttm_end_date AS period_end,
                any_value(t.ttm_start_date) AS period_start,
                any_value(t.fiscal_year) AS fiscal_year,
                any_value(t.fiscal_period) AS fiscal_period,
                {_pivot_case('t', 'ttm_value', TTM_INPUTS)}
            FROM fundamental_ttm_points t
            {sym_join_t}
            WHERE t.is_latest_revision
            GROUP BY t.security_id, t.ttm_end_date
        ),
        bal AS (
            SELECT
                s.security_id,
                s.period_end,
                {_pivot_case('s', 'value', BALANCE_INPUTS)}
            FROM fundamental_statement_points s
            {sym_join_b}
            WHERE s.is_latest_revision AND s.period_type = 'instant'
            GROUP BY s.security_id, s.period_end
        ),
        balx AS (
            SELECT
                x.security_id,
                x.period_end,
                {_pivot_case('x', 'value', XBRL_BALANCE_INPUTS)}
            FROM fundamental_xbrl_metric x
            {sym_join_x}
            WHERE x.is_latest_revision AND x.period_type = 'instant'
            GROUP BY x.security_id, x.period_end
        )
        SELECT
            ttm.*,
            bal.assets, bal.assets_av,
            bal.liabilities, bal.liabilities_av,
            bal.equity, bal.equity_av,
            bal.shares, bal.shares_av,
            balx.current_assets, balx.current_assets_av,
            balx.current_liabilities, balx.current_liabilities_av,
            balx.cash_and_equivalents, balx.cash_and_equivalents_av,
            balx.inventory, balx.inventory_av,
            balx.long_term_debt, balx.long_term_debt_av
        FROM ttm
        LEFT JOIN bal
          ON bal.security_id = ttm.security_id
         AND bal.period_end = ttm.period_end
        LEFT JOIN balx
          ON balx.security_id = ttm.security_id
         AND balx.period_end = ttm.period_end
    """
    try:
        wide = store.con.execute(sql).df()
    finally:
        if registered:
            store.con.unregister("ratio_symbol_filter")
    return _attach_prior_year(wide)


def refresh_fundamental_ratios(store: DuckDBStore, options: FundamentalRatiosOptions) -> int:
    """Recompute and replace the ratio rows for ``options.source``/``options.basis``."""
    store.initialize()
    wide = load_ratio_inputs(store, options)
    rows = compute_ratio_rows(wide, source=options.source, basis=options.basis, run_id=options.run_id)
    with store.transaction():
        store.con.execute(
            "DELETE FROM fundamental_ratios WHERE source = ? AND basis = ?",
            [options.source, options.basis],
        )
        if not rows.empty:
            insert_frame(store, rows, "fundamental_ratios", "fundamental_ratios_insert")
    return int(len(rows))


class FundamentalRatiosDataset(Dataset):
    dataset_id = "fundamental_ratios"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FundamentalRatiosOptions) -> DatasetLoadResult:
        rows = refresh_fundamental_ratios(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="fundamental_ratios",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source, "basis": options.basis},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"basis": options.basis, "ratio_codes": [d.code for d in RATIO_DEFS]},
        )
