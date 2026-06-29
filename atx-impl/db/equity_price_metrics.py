"""S14: derived daily price analytics (`equity_price_metrics`).

The cached ``equity_daily_bars`` feed holds split/dividend-adjusted daily OHLCV. This
module turns it into a typed, point-in-time analytics surface — one row per
``(security_id, trade_date)`` — with the canonical price features quant strategies
condition on: adjusted daily and log returns, the overnight gap, trailing realized
volatility (20d/60d, annualized), trailing-return momentum (21d/126d), distance from the
trailing 252-day high, and dollar volume.

Point-in-time discipline: ``as_of_date`` is the trade date and ``available_at`` is
carried from the bar. Every rolling/lag feature uses only the current and earlier bars
(returns look back, volatility/high are trailing windows), so there is no forward
leakage. Returns are computed on the adjusted close so corporate actions don't create
spurious jumps; momentum and volatility are reported in fractions / annualized fractions.

The math lives in :func:`compute_equity_price_metrics`, a pure DataFrame->DataFrame
transform unit-tested without DuckDB; :class:`EquityPriceMetricsDataset` /
:func:`refresh_equity_price_metrics` feed it the cached bars and write the result.
No network.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass

import numpy as np
import pandas as pd

from .asof import equity_price_metrics_asof  # noqa: F401  (re-exported for callers)
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check


SOURCE_NAME = "Derived daily price analytics"
DEFAULT_SOURCE = "derived_equity_price_metrics_v1"

TRADING_DAYS = 252
VOL_WINDOW_SHORT = 20
VOL_WINDOW_LONG = 60
MOMENTUM_SHORT = 21
MOMENTUM_LONG = 126
HIGH_WINDOW = 252

EQUITY_PRICE_METRIC_COLUMNS = [
    "metric_id", "source", "security_id", "symbol", "trade_date",
    "close", "adjusted_close", "volume", "dollar_volume",
    "daily_return", "log_return", "gap_return",
    "realized_vol_20d", "realized_vol_60d",
    "momentum_21d", "momentum_126d", "pct_from_high_252d",
    "is_latest_revision", "as_of_date", "available_at", "run_id",
]


@dataclass(frozen=True)
class EquityPriceMetricsOptions:
    source: str = DEFAULT_SOURCE
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


def _metric_id(source: str, security_id: str, trade_date) -> str:
    payload = "|".join(str(p) for p in (source, security_id, trade_date))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _back_adjusted_close(close: pd.Series, split_factor: pd.Series) -> pd.Series:
    """Split/dividend back-adjusted close: each day scaled by the product of all
    *future* adjustment factors. This is computed from the bar's own ``split_factor``
    (0.5 on a 2:1 split, ~0.997 on a dividend), NOT the feed's ``adjusted_close``
    column — in the cached 2012-13 sample that column is an unadjusted lagged close and
    leaves split jumps in, which would inflate returns/volatility. The cumulative-future
    product makes the series continuous across splits so returns reflect real moves.
    """
    factor = pd.to_numeric(split_factor, errors="coerce").fillna(1.0)
    rev_cumfac = factor[::-1].cumprod().shift(1).fillna(1.0)[::-1]
    return close * rev_cumfac.to_numpy()


def _derive_one_security(g: pd.DataFrame) -> pd.DataFrame:
    """Per-security price transforms over one symbol's bars (sorted by trade_date)."""
    g = g.sort_values("trade_date").reset_index(drop=True)
    close = pd.to_numeric(g["close"], errors="coerce")
    open_ = pd.to_numeric(g.get("open"), errors="coerce")
    volume = pd.to_numeric(g["volume"], errors="coerce")
    adj = _back_adjusted_close(close, g.get("split_factor", pd.Series(1.0, index=g.index)))
    adj_open = _back_adjusted_close(open_, g.get("split_factor", pd.Series(1.0, index=g.index)))

    g["close"] = close
    g["adjusted_close"] = adj
    g["volume"] = volume
    g["dollar_volume"] = close * volume
    g["daily_return"] = adj.pct_change()
    with np.errstate(divide="ignore", invalid="ignore"):
        g["log_return"] = np.log(adj / adj.shift(1))
    prev_adj = adj.shift(1)
    g["gap_return"] = adj_open / prev_adj.where(prev_adj > 0) - 1.0
    ret = g["daily_return"]
    g["realized_vol_20d"] = ret.rolling(VOL_WINDOW_SHORT, min_periods=VOL_WINDOW_SHORT).std(ddof=1) * np.sqrt(TRADING_DAYS)
    g["realized_vol_60d"] = ret.rolling(VOL_WINDOW_LONG, min_periods=VOL_WINDOW_LONG).std(ddof=1) * np.sqrt(TRADING_DAYS)
    g["momentum_21d"] = adj / adj.shift(MOMENTUM_SHORT) - 1.0
    g["momentum_126d"] = adj / adj.shift(MOMENTUM_LONG) - 1.0
    roll_high = adj.rolling(HIGH_WINDOW, min_periods=1).max()
    g["pct_from_high_252d"] = adj / roll_high.where(roll_high > 0) - 1.0
    return g


def compute_equity_price_metrics(
    bars: pd.DataFrame,
    *,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: daily bars -> typed price metric rows.

    Input carries one row per ``(security_id, trade_date)`` with adjusted/raw OHLCV and
    ``available_at``. Rolling/lag features are computed per security in date order.
    """
    if bars is None or bars.empty:
        return pd.DataFrame(columns=EQUITY_PRICE_METRIC_COLUMNS)

    out = bars.copy()
    out["trade_date"] = pd.to_datetime(out["trade_date"]).astype("datetime64[ns]")
    out["available_at"] = pd.to_datetime(out["available_at"], errors="coerce")
    if "open" not in out.columns:
        out["open"] = np.nan
    if "split_factor" not in out.columns:
        out["split_factor"] = 1.0

    derived = (
        out.groupby("security_id", group_keys=False)[out.columns.tolist()]
        .apply(_derive_one_security)
        .reset_index(drop=True)
    )

    derived["source"] = source
    derived["run_id"] = run_id
    derived["as_of_date"] = derived["trade_date"]
    derived["is_latest_revision"] = True
    derived["metric_id"] = [
        _metric_id(source, sid, td.date() if hasattr(td, "date") else td)
        for sid, td in zip(derived["security_id"], derived["trade_date"])
    ]
    derived["trade_date"] = derived["trade_date"].dt.date
    derived["as_of_date"] = derived["as_of_date"].dt.date
    if "symbol" not in derived.columns:
        derived["symbol"] = pd.NA
    return derived[EQUITY_PRICE_METRIC_COLUMNS]


_LOAD_SQL = """
    SELECT
        b.security_id,
        b.symbol,
        b.trade_date,
        b.open,
        b.close,
        b.split_factor,
        b.volume,
        b.available_at
    FROM equity_daily_bars b
    {symbol_pred}
"""


def load_price_inputs(store: DuckDBStore, options: EquityPriceMetricsOptions) -> pd.DataFrame:
    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    registered = False
    symbol_pred = ""
    if symbols:
        store.con.register(
            "eqpm_symbol_filter",
            pd.DataFrame({"symbol": sorted({str(s).strip().upper() for s in symbols})}),
        )
        registered = True
        symbol_pred = "WHERE b.symbol IN (SELECT symbol FROM eqpm_symbol_filter)"
    sql = _LOAD_SQL.format(symbol_pred=symbol_pred)
    try:
        return store.con.execute(sql).df()
    finally:
        if registered:
            store.con.unregister("eqpm_symbol_filter")


def refresh_equity_price_metrics(store: DuckDBStore, options: EquityPriceMetricsOptions) -> int:
    """Recompute and replace the price metric rows for ``options.source``."""
    store.initialize()
    inputs = load_price_inputs(store, options)
    rows = compute_equity_price_metrics(inputs, source=options.source, run_id=options.run_id)
    with store.transaction():
        store.con.execute("DELETE FROM equity_price_metrics WHERE source = ?", [options.source])
        if not rows.empty:
            insert_frame(store, rows, "equity_price_metrics", "equity_price_metrics_insert")
    return int(len(rows))


class EquityPriceMetricsDataset(Dataset):
    dataset_id = "equity_price_metrics"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: EquityPriceMetricsOptions) -> DatasetLoadResult:
        rows = refresh_equity_price_metrics(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="equity_price_metrics",
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
            details={"grain": "security_id,trade_date"},
        )
