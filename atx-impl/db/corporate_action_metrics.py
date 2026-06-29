"""S18: derived cash-dividend analytics (`corporate_action_dividend_metrics`).

The cached ``corporate_actions`` feed records per-security cash-dividend events (ex-date,
cash amount). This module joins each event to the ex-date close in ``equity_daily_bars``
and turns it into a typed, point-in-time analytics surface — one row per
``(security_id, ex_date)`` — adding the dividend signals quant strategies condition on:
spot dividend yield (cash / price), the trailing-twelve-month dividend sum and count, the
TTM dividend yield (the standard income-factor input), and year-over-year dividend growth.
This is the first *derived* layer on the corporate-actions domain and a genuine
cross-domain join (corporate actions x pricing), made possible because the cached
dividend events (2012-04..2013-03) temporally overlap the cached daily bars.

Point-in-time discipline: ``as_of_date`` is the ex-date and ``available_at`` is the later
of the dividend-inference and ex-date-bar availabilities, so a dividend appears only once
both legs are knowable. Every TTM/growth value uses only this security's current and
earlier dividends; the yield uses the ex-date close — no forward leakage.

The math lives in :func:`compute_dividend_metrics`, a pure DataFrame->DataFrame transform
unit-tested without DuckDB; :class:`CorporateActionDividendMetricsDataset` /
:func:`refresh_dividend_metrics` feed it the joined events and write the result. No network.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass

import numpy as np
import pandas as pd

from .adjustment_factors import SOURCE_NAME as ADJUSTMENT_FACTOR_SOURCE
from .asof import (  # noqa: F401  (re-exported)
    corporate_action_dividend_metrics_asof,
    corporate_action_split_metrics_asof,
)
from .connection import DuckDBStore
from .daily_adjustments import SOURCE_NAME as DAILY_ADJUSTMENT_SOURCE
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check


SOURCE_NAME = "Derived cash-dividend analytics"
DEFAULT_SOURCE = "derived_corporate_action_dividend_metrics_v1"
SPLIT_SOURCE_NAME = "Derived split adjustment reconciliation"
DEFAULT_SPLIT_SOURCE = "derived_corporate_action_split_metrics_v1"
YOY_TOLERANCE_DAYS = 45  # nearest dividend ~1 year prior (quarterly payers => +/- a quarter edge)
DIVIDEND_ACTION_TYPES = ("cash_dividend_inferred", "cash_dividend")
SPLIT_RECONCILIATION_TOLERANCE = 1e-8
# A single cash dividend never exceeds this fraction of the share price. The upstream
# `cash_dividend_inferred` rows are inferred from the ex-date price drop, which cannot
# distinguish a stock split from a dividend — a 2:1 split reads as a ~50%-of-price
# "dividend". Events whose spot yield exceeds this ceiling are split artifacts and are
# excluded so they never pollute the trailing-twelve-month dividend sum.
MAX_SINGLE_DIVIDEND_YIELD = 0.25

DIVIDEND_METRIC_COLUMNS = [
    "metric_id", "source", "security_id", "symbol", "ex_date", "record_date",
    "payable_date", "cash_amount", "close_on_ex", "dividend_yield_spot",
    "ttm_dividend", "ttm_dividend_count", "dividend_yield_ttm", "dividend_growth_yoy",
    "dividend_ordinal", "is_latest_revision", "as_of_date", "available_at", "run_id",
]

SPLIT_METRIC_COLUMNS = [
    "split_metric_id", "source", "factor_source", "daily_adjustment_source",
    "bar_source", "security_id", "symbol", "ex_date", "event_ref_id",
    "source_action_source", "classification_reason", "factor_price", "factor_shares",
    "ratio_numerator", "ratio_denominator", "split_ratio", "pre_trade_date",
    "post_trade_date", "pre_raw_close", "post_raw_close", "pre_split_adjusted_close",
    "post_split_adjusted_close", "raw_close_return", "split_adjusted_return",
    "pre_split_price_factor", "post_split_price_factor", "observed_factor_price_step",
    "factor_price_error", "pre_split_share_factor", "post_split_share_factor",
    "observed_factor_share_step", "factor_share_error", "reconciliation_status",
    "is_reconciled", "is_latest_revision", "as_of_date", "available_at", "run_id",
]


@dataclass(frozen=True)
class CorporateActionDividendMetricsOptions:
    source: str = DEFAULT_SOURCE
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


@dataclass(frozen=True)
class CorporateActionSplitMetricsOptions:
    source: str = DEFAULT_SPLIT_SOURCE
    factor_source: str = ADJUSTMENT_FACTOR_SOURCE
    daily_adjustment_source: str = DAILY_ADJUSTMENT_SOURCE
    bar_source: str | None = None
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


def _metric_id(source: str, security_id: str, ex_date) -> str:
    payload = "|".join(str(p) for p in (source, security_id, ex_date))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _split_metric_id(
    source: str,
    factor_source: str,
    daily_adjustment_source: str,
    bar_source: str | None,
    security_id: str,
    event_ref_id: str,
    ex_date,
) -> str:
    payload = "|".join(
        str(p)
        for p in (
            source,
            factor_source,
            daily_adjustment_source,
            bar_source or "",
            security_id,
            event_ref_id,
            ex_date,
        )
    )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _yoy_prior_cash(g: pd.DataFrame) -> np.ndarray:
    """Cash amount of the dividend nearest ~1 year before each event (within tolerance)."""
    lookup = g[["ex_date"]].copy()
    lookup["lookup_date"] = lookup["ex_date"] - pd.Timedelta(days=365)
    base = g[["ex_date", "cash_amount"]].rename(columns={"ex_date": "base_date", "cash_amount": "base_cash"})
    merged = pd.merge_asof(
        lookup.sort_values("lookup_date"),
        base.sort_values("base_date"),
        left_on="lookup_date",
        right_on="base_date",
        direction="nearest",
        tolerance=pd.Timedelta(days=YOY_TOLERANCE_DAYS),
    )
    return merged.set_index("ex_date")["base_cash"].reindex(g["ex_date"]).to_numpy()


def _derive_one_security(g: pd.DataFrame) -> pd.DataFrame:
    """Per-security dividend transforms over one symbol's events (sorted by ex_date)."""
    g = g.sort_values("ex_date").reset_index(drop=True)
    cash = pd.to_numeric(g["cash_amount"], errors="coerce")
    close = pd.to_numeric(g["close_on_ex"], errors="coerce")
    g["cash_amount"] = cash
    g["close_on_ex"] = close
    g["dividend_ordinal"] = np.arange(1, len(g) + 1)
    safe_close = close.where(close > 0)
    g["dividend_yield_spot"] = cash / safe_close

    # Trailing-twelve-month dividend sum / count over a 365-day rolling window on the
    # ex-date index (inclusive of the current event).
    rolled = pd.Series(cash.to_numpy(dtype="float64"), index=pd.DatetimeIndex(g["ex_date"]))
    g["ttm_dividend"] = rolled.rolling("365D").sum().to_numpy()
    g["ttm_dividend_count"] = rolled.rolling("365D").count().to_numpy().astype("int64")
    g["dividend_yield_ttm"] = g["ttm_dividend"].to_numpy() / safe_close.to_numpy()

    base_cash = _yoy_prior_cash(g)
    cur = cash.to_numpy(dtype="float64")
    with np.errstate(divide="ignore", invalid="ignore"):
        growth = (cur - base_cash) / base_cash
    g["dividend_growth_yoy"] = np.where(np.isnan(base_cash) | (base_cash <= 0), np.nan, growth)
    return g


def compute_dividend_metrics(
    events: pd.DataFrame,
    *,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: cash-dividend events (+ ex-date close) -> typed metric rows."""
    if events is None or events.empty:
        return pd.DataFrame(columns=DIVIDEND_METRIC_COLUMNS)

    out = events.copy()
    out["ex_date"] = pd.to_datetime(out["ex_date"]).astype("datetime64[ns]")
    out["available_at"] = pd.to_datetime(out["available_at"], errors="coerce")
    for col in ("symbol", "record_date", "payable_date"):
        if col not in out.columns:
            out[col] = pd.NA

    derived = (
        out.groupby("security_id", group_keys=False)[out.columns.tolist()]
        .apply(_derive_one_security)
        .reset_index(drop=True)
    )

    derived["source"] = source
    derived["run_id"] = run_id
    derived["as_of_date"] = derived["ex_date"]
    derived["is_latest_revision"] = True
    derived["metric_id"] = [
        _metric_id(source, sid, ed.date() if hasattr(ed, "date") else ed)
        for sid, ed in zip(derived["security_id"], derived["ex_date"])
    ]
    derived["ex_date"] = derived["ex_date"].dt.date
    derived["as_of_date"] = derived["as_of_date"].dt.date
    return derived[DIVIDEND_METRIC_COLUMNS]


def _safe_divide(numerator: pd.Series, denominator: pd.Series) -> pd.Series:
    denom = pd.to_numeric(denominator, errors="coerce")
    numer = pd.to_numeric(numerator, errors="coerce")
    return numer / denom.where(denom > 0)


def compute_split_metrics(
    events: pd.DataFrame,
    *,
    source: str = DEFAULT_SPLIT_SOURCE,
    factor_source: str = ADJUSTMENT_FACTOR_SOURCE,
    daily_adjustment_source: str = DAILY_ADJUSTMENT_SOURCE,
    run_id: str | None = None,
    tolerance: float = SPLIT_RECONCILIATION_TOLERANCE,
) -> pd.DataFrame:
    """Pure transform: split events + adjacent daily factors -> reconciliation rows."""
    if events is None or events.empty:
        return pd.DataFrame(columns=SPLIT_METRIC_COLUMNS)

    out = events.copy()
    for col in ("ex_date", "pre_trade_date", "post_trade_date"):
        if col in out.columns:
            out[col] = pd.to_datetime(out[col], errors="coerce")
        else:
            out[col] = pd.NaT
    for col in ("event_available_at", "pre_available_at", "post_available_at"):
        if col in out.columns:
            out[col] = pd.to_datetime(out[col], errors="coerce")
        else:
            out[col] = pd.NaT
    for col in (
        "factor_price", "factor_shares", "ratio_numerator", "ratio_denominator",
        "pre_raw_close", "post_raw_close", "pre_split_adjusted_close",
        "post_split_adjusted_close", "pre_split_price_factor", "post_split_price_factor",
        "pre_split_share_factor", "post_split_share_factor",
    ):
        if col not in out.columns:
            out[col] = np.nan
        out[col] = pd.to_numeric(out[col], errors="coerce")
    for col in ("bar_source", "symbol", "source_action_source", "classification_reason"):
        if col not in out.columns:
            out[col] = pd.NA

    out["source"] = source
    out["factor_source"] = factor_source
    out["daily_adjustment_source"] = daily_adjustment_source
    out["split_ratio"] = out["factor_shares"]
    out["observed_factor_price_step"] = _safe_divide(
        out["pre_split_price_factor"],
        out["post_split_price_factor"],
    )
    out["observed_factor_share_step"] = _safe_divide(
        out["pre_split_share_factor"],
        out["post_split_share_factor"],
    )
    out["factor_price_error"] = out["observed_factor_price_step"] - out["factor_price"]
    out["factor_share_error"] = out["observed_factor_share_step"] - out["factor_shares"]
    out["raw_close_return"] = _safe_divide(out["post_raw_close"], out["pre_raw_close"]) - 1.0
    out["split_adjusted_return"] = (
        _safe_divide(out["post_split_adjusted_close"], out["pre_split_adjusted_close"]) - 1.0
    )

    missing_daily = out["observed_factor_price_step"].isna() | out["observed_factor_share_step"].isna()
    mismatch = (
        out["factor_price_error"].abs().gt(tolerance)
        | out["factor_share_error"].abs().gt(tolerance)
    ).fillna(False)
    out["reconciliation_status"] = np.select(
        [missing_daily, mismatch],
        ["MISSING_DAILY_FACTOR", "MISMATCH"],
        default="RECONCILED",
    )
    out["is_reconciled"] = out["reconciliation_status"].eq("RECONCILED")
    out["is_latest_revision"] = True
    out["as_of_date"] = out["ex_date"]
    out["available_at"] = out[["event_available_at", "pre_available_at", "post_available_at"]].max(axis=1)
    out["run_id"] = run_id
    out["split_metric_id"] = [
        _split_metric_id(
            source,
            factor_source,
            daily_adjustment_source,
            bar,
            sid,
            ref,
            ed.date() if hasattr(ed, "date") else ed,
        )
        for bar, sid, ref, ed in zip(out["bar_source"], out["security_id"], out["event_ref_id"], out["ex_date"])
    ]

    for col in ("ex_date", "pre_trade_date", "post_trade_date", "as_of_date"):
        out[col] = out[col].dt.date
    return out[SPLIT_METRIC_COLUMNS]


def _load_sql(action_types: tuple[str, ...]) -> str:
    placeholders = ", ".join(["?"] * len(action_types))
    max_yield = MAX_SINGLE_DIVIDEND_YIELD
    return f"""
        SELECT
            ca.security_id,
            ca.symbol,
            ca.ex_date,
            ca.record_date,
            ca.payable_date,
            ca.cash_amount,
            b.close AS close_on_ex,
            greatest(ca.available_at, coalesce(b.available_at, ca.available_at)) AS available_at
        FROM corporate_actions ca
        LEFT JOIN equity_daily_bars b
          ON b.security_id = ca.security_id AND b.trade_date = ca.ex_date
        WHERE ca.action_type IN ({placeholders})
          AND ca.cash_amount IS NOT NULL
          AND ca.cash_amount > 0
          AND ca.available_at IS NOT NULL
          -- Drop split artifacts misinferred as dividends (a real dividend is a small
          -- fraction of the price); keep events with no ex-date close (yield unverifiable).
          AND (b.close IS NULL OR b.close <= 0 OR ca.cash_amount <= {max_yield} * b.close)
          {{symbol_pred}}
    """


def load_dividend_inputs(store: DuckDBStore, options: CorporateActionDividendMetricsOptions) -> pd.DataFrame:
    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    registered = False
    symbol_pred = ""
    if symbols:
        store.con.register(
            "cadiv_symbol_filter",
            pd.DataFrame({"symbol": sorted({str(s).strip().upper() for s in symbols})}),
        )
        registered = True
        symbol_pred = "AND ca.symbol IN (SELECT symbol FROM cadiv_symbol_filter)"
    sql = _load_sql(DIVIDEND_ACTION_TYPES).format(symbol_pred=symbol_pred)
    try:
        return store.con.execute(sql, list(DIVIDEND_ACTION_TYPES)).df()
    finally:
        if registered:
            store.con.unregister("cadiv_symbol_filter")


def refresh_dividend_metrics(store: DuckDBStore, options: CorporateActionDividendMetricsOptions) -> int:
    """Recompute and replace the dividend metric rows for ``options.source``."""
    store.initialize()
    inputs = load_dividend_inputs(store, options)
    rows = compute_dividend_metrics(inputs, source=options.source, run_id=options.run_id)
    with store.transaction():
        store.con.execute("DELETE FROM corporate_action_dividend_metrics WHERE source = ?", [options.source])
        if not rows.empty:
            insert_frame(store, rows, "corporate_action_dividend_metrics", "corporate_action_dividend_metrics_insert")
    return int(len(rows))


def _load_split_sql() -> str:
    return """
        WITH params AS (
            SELECT
                ? AS factor_source,
                ? AS daily_adjustment_source,
                ? AS bar_source_filter
        ),
        events AS (
            SELECT
                a.security_id,
                a.symbol,
                a.ex_date,
                a.event_ref_id,
                a.source_action_source,
                a.classification_reason,
                a.factor_price,
                a.factor_shares,
                a.ratio_numerator,
                a.ratio_denominator,
                a.available_at AS event_available_at
            FROM adjustment_factor_history a
            CROSS JOIN params p
            WHERE a.source = p.factor_source
              AND a.event_type = 'SPLIT'
              AND a.factor_price > 0
              AND a.factor_shares > 0
              AND a.available_at IS NOT NULL
              {symbol_pred}
        ),
        latest_daily AS (
            SELECT d.*
            FROM daily_adjustment_factors d
            CROSS JOIN params p
            JOIN (
                SELECT
                    d2.source,
                    d2.factor_source,
                    d2.bar_source,
                    max(d2.as_of_date) AS max_as_of_date
                FROM daily_adjustment_factors d2
                CROSS JOIN params p2
                WHERE d2.source = p2.daily_adjustment_source
                  AND d2.factor_source = p2.factor_source
                  AND (p2.bar_source_filter IS NULL OR d2.bar_source = p2.bar_source_filter)
                GROUP BY 1, 2, 3
            ) snap
              ON snap.source = d.source
             AND snap.factor_source = d.factor_source
             AND snap.bar_source = d.bar_source
             AND snap.max_as_of_date = d.as_of_date
            WHERE d.source = p.daily_adjustment_source
              AND d.factor_source = p.factor_source
              AND (p.bar_source_filter IS NULL OR d.bar_source = p.bar_source_filter)
        ),
        event_bar_sources AS (
            SELECT e.event_ref_id, d.bar_source
            FROM events e
            JOIN latest_daily d ON d.security_id = e.security_id
            GROUP BY 1, 2
            UNION ALL
            SELECT e.event_ref_id, NULL AS bar_source
            FROM events e
            WHERE NOT EXISTS (
                SELECT 1 FROM latest_daily d WHERE d.security_id = e.security_id
            )
        ),
        pre_ranked AS (
            SELECT
                e.event_ref_id,
                bs.bar_source,
                d.trade_date,
                d.raw_close,
                d.split_adjusted_close,
                d.split_price_factor,
                d.split_share_factor,
                d.available_at,
                row_number() OVER (
                    PARTITION BY e.event_ref_id, bs.bar_source
                    ORDER BY d.trade_date DESC NULLS LAST
                ) AS rn
            FROM events e
            JOIN event_bar_sources bs ON bs.event_ref_id = e.event_ref_id
            LEFT JOIN latest_daily d
              ON d.security_id = e.security_id
             AND d.bar_source = bs.bar_source
             AND d.trade_date < e.ex_date
        ),
        post_ranked AS (
            SELECT
                e.event_ref_id,
                bs.bar_source,
                d.trade_date,
                d.raw_close,
                d.split_adjusted_close,
                d.split_price_factor,
                d.split_share_factor,
                d.available_at,
                row_number() OVER (
                    PARTITION BY e.event_ref_id, bs.bar_source
                    ORDER BY d.trade_date ASC NULLS LAST
                ) AS rn
            FROM events e
            JOIN event_bar_sources bs ON bs.event_ref_id = e.event_ref_id
            LEFT JOIN latest_daily d
              ON d.security_id = e.security_id
             AND d.bar_source = bs.bar_source
             AND d.trade_date >= e.ex_date
        ),
        pre AS (
            SELECT * FROM pre_ranked WHERE rn = 1
        ),
        post AS (
            SELECT * FROM post_ranked WHERE rn = 1
        )
        SELECT
            e.*,
            bs.bar_source,
            pre.trade_date AS pre_trade_date,
            post.trade_date AS post_trade_date,
            pre.raw_close AS pre_raw_close,
            post.raw_close AS post_raw_close,
            pre.split_adjusted_close AS pre_split_adjusted_close,
            post.split_adjusted_close AS post_split_adjusted_close,
            pre.split_price_factor AS pre_split_price_factor,
            post.split_price_factor AS post_split_price_factor,
            pre.split_share_factor AS pre_split_share_factor,
            post.split_share_factor AS post_split_share_factor,
            pre.available_at AS pre_available_at,
            post.available_at AS post_available_at
        FROM events e
        JOIN event_bar_sources bs ON bs.event_ref_id = e.event_ref_id
        LEFT JOIN pre
          ON pre.event_ref_id = e.event_ref_id
         AND coalesce(pre.bar_source, '') = coalesce(bs.bar_source, '')
        LEFT JOIN post
          ON post.event_ref_id = e.event_ref_id
         AND coalesce(post.bar_source, '') = coalesce(bs.bar_source, '')
    """


def load_split_inputs(store: DuckDBStore, options: CorporateActionSplitMetricsOptions) -> pd.DataFrame:
    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    registered = False
    symbol_pred = ""
    if symbols:
        store.con.register(
            "casplit_symbol_filter",
            pd.DataFrame({"symbol": sorted({str(s).strip().upper() for s in symbols})}),
        )
        registered = True
        symbol_pred = "AND a.symbol IN (SELECT symbol FROM casplit_symbol_filter)"
    sql = _load_split_sql().format(symbol_pred=symbol_pred)
    try:
        return store.con.execute(
            sql,
            [options.factor_source, options.daily_adjustment_source, options.bar_source],
        ).df()
    finally:
        if registered:
            store.con.unregister("casplit_symbol_filter")


def refresh_split_metrics(store: DuckDBStore, options: CorporateActionSplitMetricsOptions) -> int:
    """Recompute and replace the split reconciliation rows for ``options.source``."""
    store.initialize()
    inputs = load_split_inputs(store, options)
    rows = compute_split_metrics(
        inputs,
        source=options.source,
        factor_source=options.factor_source,
        daily_adjustment_source=options.daily_adjustment_source,
        run_id=options.run_id,
    )
    with store.transaction():
        store.con.execute("DELETE FROM corporate_action_split_metrics WHERE source = ?", [options.source])
        if not rows.empty:
            insert_frame(store, rows, "corporate_action_split_metrics", "corporate_action_split_metrics_insert")
    return int(len(rows))


class CorporateActionDividendMetricsDataset(Dataset):
    dataset_id = "corporate_action_dividend_metrics"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: CorporateActionDividendMetricsOptions) -> DatasetLoadResult:
        rows = refresh_dividend_metrics(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="corporate_action_dividend_metrics",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"grain": "security_id,ex_date"},
        )


class CorporateActionSplitMetricsDataset(Dataset):
    dataset_id = "corporate_action_split_metrics"
    source_name = SPLIT_SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: CorporateActionSplitMetricsOptions) -> DatasetLoadResult:
        rows = refresh_split_metrics(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="corporate_action_split_metrics",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "source": options.source,
                "factor_source": options.factor_source,
                "daily_adjustment_source": options.daily_adjustment_source,
                "bar_source": options.bar_source,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"grain": "security_id,ex_date,event_ref_id,bar_source"},
        )
