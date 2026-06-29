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

from .asof import corporate_action_dividend_metrics_asof  # noqa: F401  (re-exported)
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check


SOURCE_NAME = "Derived cash-dividend analytics"
DEFAULT_SOURCE = "derived_corporate_action_dividend_metrics_v1"
YOY_TOLERANCE_DAYS = 45  # nearest dividend ~1 year prior (quarterly payers => +/- a quarter edge)
DIVIDEND_ACTION_TYPES = ("cash_dividend_inferred", "cash_dividend")
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


@dataclass(frozen=True)
class CorporateActionDividendMetricsOptions:
    source: str = DEFAULT_SOURCE
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


def _metric_id(source: str, security_id: str, ex_date) -> str:
    payload = "|".join(str(p) for p in (source, security_id, ex_date))
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
