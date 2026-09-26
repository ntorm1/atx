"""S14: derived daily price analytics (`equity_price_metrics`).

The cached ``equity_daily_bars`` feed holds raw OHLCV and adjusted close. This
module turns it into a typed, point-in-time analytics surface — one row per
``(security_id, trade_date)`` — with the canonical price features quant strategies
condition on: adjusted daily and log returns, the overnight gap, trailing realized
volatility (20d/60d, annualized), trailing-return momentum (21d/126d), distance from the
trailing 252-day high, dollar volume, trailing average dollar volume (ADV), Amihud
(2002) illiquidity, trailing maximum drawdown, downside deviation, market-relative
risk factors, and daily cross-sectional percentile ranks.

Point-in-time discipline: ``as_of_date`` is the trade date and ``available_at`` is
carried from the bar, or delayed to the latest same-day bar used in the equal-weight
market proxy. Every rolling/lag feature uses only the current and earlier bars (returns
look back, volatility/high are trailing windows), so there is no forward leakage. Returns
are computed on the adjusted close so corporate actions don't create spurious jumps;
momentum and volatility are reported in fractions / annualized fractions.

The released dataset (``price-metrics-1d``) is built by :func:`refresh_equity_price_metrics`
in bounded SQL over one picked bar per ``(security_id, trade_date)`` (the shared bar pick)
whose adjusted close is the ``vendor_artifact_repaired`` series (:mod:`atx_db._vendor_artifact`,
:data:`ADJ_CLOSE_BASIS`), so the vendor's 2021-01-04 factor-decrease artifact enters no return,
window or the equal-weight market proxy. :func:`compute_equity_price_metrics` is the same math
as a pure DataFrame->DataFrame transform over the adjusted closes the caller passes (unit-tested
without DuckDB). No network.
"""
from __future__ import annotations

import hashlib
import datetime as dt
from dataclasses import dataclass

import numpy as np
import pandas as pd

from .asof import equity_price_metrics_asof  # noqa: F401  (re-exported for callers)
from ._bulk_publication import publish_validated_shadow
from ._vendor_artifact import REPAIR_VERSION, bar_pick_order_sql, repaired_bars_sql
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check


SOURCE_NAME = "Derived daily price analytics"
DEFAULT_SOURCE = "derived_equity_price_metrics_v1"
#: Adjusted-close basis of the refresh (0.13 / I1): every return, momentum, volatility, drawdown,
#: high and the equal-weight market proxy read the vendor_artifact_repaired adjusted close of one
#: picked bar per (security_id, trade_date). The table has no version column (rows keep the source
#: id, so metric_id is unchanged); the basis is recorded in the refresh's quality-check details.
ADJ_CLOSE_BASIS = REPAIR_VERSION

TRADING_DAYS = 252
VOL_WINDOW_SHORT = 20
VOL_WINDOW_LONG = 60
MOMENTUM_SHORT = 21
MOMENTUM_LONG = 126
HIGH_WINDOW = 252
LIQUIDITY_WINDOW = 21
DRAWDOWN_WINDOW = 126
DOWNSIDE_WINDOW = 60
MARKET_RISK_WINDOW = 60
# Amihud (2002) ILLIQ is averaged |return| per dollar of volume; dollar_volume here is
# in raw dollars, so the ratio is tiny. Scale by 1e9 to express price impact per $1B
# traded, keeping values in a human-readable range.
AMIHUD_SCALE = 1e9

EQUITY_PRICE_METRIC_COLUMNS = [
    "metric_id", "source", "security_id", "symbol", "trade_date",
    "close", "adjusted_close", "volume", "dollar_volume",
    "daily_return", "log_return", "gap_return",
    "realized_vol_20d", "realized_vol_60d",
    "momentum_21d", "momentum_126d", "pct_from_high_252d",
    "avg_dollar_volume_21d", "amihud_illiquidity_21d",
    "max_drawdown_126d", "downside_deviation_60d",
    "market_return_ew", "beta_60d", "market_correlation_60d", "idiosyncratic_vol_60d",
    "daily_return_cs_pct_rank", "momentum_21d_cs_pct_rank",
    "realized_vol_20d_cs_pct_rank", "dollar_volume_cs_pct_rank",
    "amihud_illiquidity_21d_cs_pct_rank",
    "is_latest_revision", "as_of_date", "available_at", "run_id",
]

CROSS_SECTIONAL_RANK_SPECS = (
    ("daily_return", "daily_return_cs_pct_rank"),
    ("momentum_21d", "momentum_21d_cs_pct_rank"),
    ("realized_vol_20d", "realized_vol_20d_cs_pct_rank"),
    ("dollar_volume", "dollar_volume_cs_pct_rank"),
    ("amihud_illiquidity_21d", "amihud_illiquidity_21d_cs_pct_rank"),
)


@dataclass(frozen=True)
class EquityPriceMetricsOptions:
    source: str = DEFAULT_SOURCE
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None
    # This is an operational eligibility cutoff, not a claim that the raw feed
    # carries certified historical source vintages.  A backfill uses the bars
    # known by the cutoff timestamp and recomputes their complete prior history.
    as_of_date: dt.date | None = None


def _metric_id(source: str, security_id: str, trade_date) -> str:
    payload = "|".join(str(p) for p in (source, security_id, trade_date))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _derive_one_security(g: pd.DataFrame) -> pd.DataFrame:
    """Per-security price transforms over one symbol's bars (sorted by trade_date)."""
    g = g.sort_values("trade_date").reset_index(drop=True)
    close = pd.to_numeric(g["close"], errors="coerce")
    open_ = pd.to_numeric(g.get("open"), errors="coerce")
    volume = pd.to_numeric(g["volume"], errors="coerce")
    adj = pd.to_numeric(g["adjusted_close"], errors="coerce")
    adj = adj.where(np.isfinite(adj) & (adj > 0))
    scale = adj / close.where(np.isfinite(close) & (close > 0))
    adj_open = open_.where(np.isfinite(open_) & (open_ > 0)) * scale
    adj_open = adj_open.where(np.isfinite(adj_open) & (adj_open > 0))

    g["close"] = close
    g["adjusted_close"] = adj
    g["volume"] = volume
    g["dollar_volume"] = close * volume
    g["daily_return"] = adj.pct_change(fill_method=None)
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
    # Liquidity factors: trailing average dollar volume (ADV) and Amihud (2002)
    # illiquidity = trailing mean of |daily_return| / dollar_volume. Both are
    # backward-looking 21-day windows, so they stay point-in-time safe.
    dollar_volume = g["dollar_volume"]
    g["avg_dollar_volume_21d"] = dollar_volume.rolling(LIQUIDITY_WINDOW, min_periods=LIQUIDITY_WINDOW).mean()
    with np.errstate(divide="ignore", invalid="ignore"):
        daily_illiq = (ret.abs() / dollar_volume.where(dollar_volume > 0)).replace([np.inf, -np.inf], np.nan)
    g["amihud_illiquidity_21d"] = (
        daily_illiq.rolling(LIQUIDITY_WINDOW, min_periods=LIQUIDITY_WINDOW).mean() * AMIHUD_SCALE
    )
    # Risk features: trailing-126d maximum drawdown from the running (expanding)
    # peak, and trailing-60d annualized downside deviation (Sortino denominator,
    # MAR=0). Both are backward-looking -> point-in-time safe. The 126-day
    # drawdown window fits the cached ~245-bar price sample (a 252-day window
    # never fills); it widens naturally once longer price history is loaded.
    running_peak = adj.cummax()
    drawdown = adj / running_peak.where(running_peak > 0) - 1.0
    g["max_drawdown_126d"] = drawdown.rolling(DRAWDOWN_WINDOW, min_periods=DRAWDOWN_WINDOW).min()
    downside = ret.clip(upper=0.0)
    g["downside_deviation_60d"] = (
        np.sqrt((downside ** 2).rolling(DOWNSIDE_WINDOW, min_periods=DOWNSIDE_WINDOW).mean()) * np.sqrt(TRADING_DAYS)
    )
    # The running peak consumes all preceding bars, including late revisions.
    g["available_at"] = g["available_at"].cummax()
    return g


def _derive_market_relative_one_security(g: pd.DataFrame) -> pd.DataFrame:
    """Rolling one-factor risk features against the loaded equal-weight market proxy."""
    g = g.sort_values("trade_date").reset_index(drop=True)
    ret = pd.to_numeric(g["daily_return"], errors="coerce")
    market = pd.to_numeric(g["market_return_ew"], errors="coerce")

    cov = ret.rolling(MARKET_RISK_WINDOW, min_periods=MARKET_RISK_WINDOW).cov(market)
    market_var = market.rolling(MARKET_RISK_WINDOW, min_periods=MARKET_RISK_WINDOW).var(ddof=1)
    ret_var = ret.rolling(MARKET_RISK_WINDOW, min_periods=MARKET_RISK_WINDOW).var(ddof=1)
    market_std = market.rolling(MARKET_RISK_WINDOW, min_periods=MARKET_RISK_WINDOW).std(ddof=1)
    ret_std = ret.rolling(MARKET_RISK_WINDOW, min_periods=MARKET_RISK_WINDOW).std(ddof=1)

    with np.errstate(divide="ignore", invalid="ignore"):
        beta = cov / market_var.where(market_var > 0)
        corr = cov / (market_std * ret_std).where((market_std > 0) & (ret_std > 0))
        residual_var = ret_var - (cov ** 2 / market_var.where(market_var > 0))

    g["beta_60d"] = beta.replace([np.inf, -np.inf], np.nan)
    g["market_correlation_60d"] = corr.clip(-1.0, 1.0).replace([np.inf, -np.inf], np.nan)
    g["idiosyncratic_vol_60d"] = (
        np.sqrt(residual_var.clip(lower=0.0)) * np.sqrt(TRADING_DAYS)
    ).replace([np.inf, -np.inf], np.nan)
    g["available_at"] = g["available_at"].cummax()
    return g


def _attach_market_relative_features(derived: pd.DataFrame) -> pd.DataFrame:
    """Add equal-weight market proxy and rolling risk features.

    The proxy is built from the rows present in ``derived``. Production full-universe
    runs therefore use the loaded bar universe; symbol-filtered runs intentionally scope
    the proxy to the filtered calculation universe.
    """
    if derived.empty:
        return derived

    market = (
        derived.groupby("trade_date", as_index=False)
        .agg(
            market_return_ew=("daily_return", "mean"),
            market_available_at=("available_at", "max"),
        )
    )
    out = derived.merge(market, on="trade_date", how="left")
    out["available_at"] = pd.concat(
        [out["available_at"], out["market_available_at"]],
        axis=1,
    ).max(axis=1)
    out = out.drop(columns=["market_available_at"])
    return (
        out.groupby("security_id", group_keys=False)[out.columns.tolist()]
        .apply(_derive_market_relative_one_security)
        .reset_index(drop=True)
    )


def _attach_cross_sectional_ranks(derived: pd.DataFrame) -> pd.DataFrame:
    """Same-day percentile ranks over the loaded calculation universe."""
    if derived.empty:
        return derived
    out = derived.copy()
    by_date = out.groupby("trade_date", sort=False)
    for value_col, rank_col in CROSS_SECTIONAL_RANK_SPECS:
        out[rank_col] = by_date[value_col].rank(method="average", pct=True)
    return out


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
    if "adjusted_close" not in out.columns:
        out["adjusted_close"] = np.nan

    derived = (
        out.groupby("security_id", group_keys=False)[out.columns.tolist()]
        .apply(_derive_one_security)
        .reset_index(drop=True)
    )
    derived = _attach_market_relative_features(derived)
    derived = _attach_cross_sectional_ranks(derived)

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
        b.adjusted_close,
        b.volume,
        b.available_at
    FROM equity_daily_bars b
    {symbol_pred}
"""


def load_price_inputs(store: DuckDBStore, options: EquityPriceMetricsOptions) -> pd.DataFrame:
    """Load all symbols only for None; an explicit empty scope loads no rows."""
    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    registered = False
    symbol_pred = ""
    if options.symbols is not None and not symbols:
        symbol_pred = "WHERE FALSE"
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


_BARS_STAGE_TABLE = "equity_price_metrics_bars_stage"


def _bars_sql(symbol_pred: str) -> str:
    """Stage 1: one picked bar per (security_id, trade_date) visible at the cutoff and its
    ``vendor_artifact_repaired`` adjusted close. Binds ``(cutoff, cutoff, cutoff_at, cutoff_at)``.

    The pick is the shared total order of every bar builder (with the share count the repair
    reads), then volume and open (the columns only this dataset reads) so tied revisions give one
    row on every run. The repair reads the picked bars' full history (every run rebuilds the
    whole source).
    """
    return f"""
CREATE OR REPLACE TABLE {_BARS_STAGE_TABLE} AS
WITH picked AS (
    SELECT b.security_id, b.symbol, b.trade_date, b.open, b.close, b.adjusted_close, b.volume,
           b.shares_outstanding, b.available_at
    FROM equity_daily_bars b
    WHERE (? IS NULL OR b.trade_date <= ?)
      AND (? IS NULL OR b.available_at <= ?)
      AND ({symbol_pred})
    QUALIFY row_number() OVER (
        PARTITION BY b.security_id, b.trade_date
        ORDER BY {bar_pick_order_sql('b', with_shares=True)}, b.volume DESC NULLS LAST, b.open DESC NULLS LAST
    ) = 1
)
SELECT security_id, symbol, trade_date, open, close, adjusted_close * va_multiplier AS adjusted_close,
       volume, available_at
FROM ({repaired_bars_sql('picked', shares='shares_outstanding')}) repaired
"""


_REFRESH_SQL = f"""
CREATE OR REPLACE TABLE equity_price_metrics_bulk_stage AS
WITH inputs AS (
    SELECT security_id, symbol, trade_date, open, close, adjusted_close, volume, available_at
    FROM {_BARS_STAGE_TABLE}
), normalized AS (
    SELECT *,
        CASE WHEN isfinite(adjusted_close) AND adjusted_close > 0
             THEN adjusted_close END AS adj,
        CASE WHEN isfinite(close) THEN close END AS numeric_close,
        CASE WHEN isfinite(open) THEN open END AS numeric_open,
        CASE WHEN isfinite(volume) THEN volume END AS numeric_volume
    FROM inputs
), lagged AS (
    SELECT *,
        lag(adj) OVER security_window AS prior_adj,
        max(available_at) OVER security_window AS security_available_at,
        max(adj) OVER security_window AS running_high
    FROM normalized
    WINDOW security_window AS (
        PARTITION BY security_id ORDER BY trade_date
        ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
    )
), returns AS (
    SELECT *,
        numeric_close * numeric_volume AS dollar_volume,
        CASE WHEN adj IS NOT NULL AND prior_adj IS NOT NULL AND prior_adj > 0
             THEN adj / prior_adj - 1.0 END AS daily_return,
        CASE WHEN adj IS NOT NULL AND prior_adj IS NOT NULL AND prior_adj > 0
             THEN ln(adj / prior_adj) END AS log_return,
        CASE WHEN numeric_open > 0 AND numeric_close > 0 AND prior_adj > 0
                  AND adj IS NOT NULL
             THEN (numeric_open * adj / numeric_close) / prior_adj - 1.0 END AS gap_return,
        CASE WHEN running_high > 0 THEN adj / running_high - 1.0 END AS drawdown
    FROM lagged
), local_metrics AS (
    SELECT *,
        CASE WHEN count(daily_return) OVER trailing_20 = 20
             THEN stddev_samp(daily_return) OVER trailing_20 * sqrt(252.0) END AS realized_vol_20d,
        CASE WHEN count(daily_return) OVER trailing_60 = 60
             THEN stddev_samp(daily_return) OVER trailing_60 * sqrt(252.0) END AS realized_vol_60d,
        CASE WHEN lag(adj, 21) OVER security_order > 0 AND adj IS NOT NULL
             THEN adj / lag(adj, 21) OVER security_order - 1.0 END AS momentum_21d,
        CASE WHEN lag(adj, 126) OVER security_order > 0 AND adj IS NOT NULL
             THEN adj / lag(adj, 126) OVER security_order - 1.0 END AS momentum_126d,
        max(adj) OVER trailing_252 AS rolling_high_252,
        CASE WHEN count(dollar_volume) OVER trailing_21 = 21
             THEN avg(dollar_volume) OVER trailing_21 END AS avg_dollar_volume_21d,
        CASE WHEN count(CASE WHEN dollar_volume > 0 THEN abs(daily_return) / dollar_volume END) OVER trailing_21 = 21
             THEN avg(CASE WHEN dollar_volume > 0 THEN abs(daily_return) / dollar_volume END) OVER trailing_21 * 1000000000.0 END AS amihud_illiquidity_21d,
        CASE WHEN count(drawdown) OVER trailing_126 = 126
             THEN min(drawdown) OVER trailing_126 END AS max_drawdown_126d,
        CASE WHEN count(daily_return) OVER trailing_60 = 60
             THEN sqrt(avg(power(least(daily_return, 0.0), 2)) OVER trailing_60) * sqrt(252.0) END AS downside_deviation_60d
    FROM returns
    WINDOW
        security_order AS (PARTITION BY security_id ORDER BY trade_date),
        trailing_20 AS (PARTITION BY security_id ORDER BY trade_date ROWS BETWEEN 19 PRECEDING AND CURRENT ROW),
        trailing_21 AS (PARTITION BY security_id ORDER BY trade_date ROWS BETWEEN 20 PRECEDING AND CURRENT ROW),
        trailing_60 AS (PARTITION BY security_id ORDER BY trade_date ROWS BETWEEN 59 PRECEDING AND CURRENT ROW),
        trailing_126 AS (PARTITION BY security_id ORDER BY trade_date ROWS BETWEEN 125 PRECEDING AND CURRENT ROW),
        trailing_252 AS (PARTITION BY security_id ORDER BY trade_date ROWS BETWEEN 251 PRECEDING AND CURRENT ROW)
), market AS (
    SELECT trade_date, avg(daily_return) AS market_return_ew,
           max(security_available_at) AS market_available_at
    FROM local_metrics GROUP BY trade_date
), market_joined AS (
    SELECT l.*, m.market_return_ew,
           max(greatest(l.security_available_at, m.market_available_at)) OVER (
               PARTITION BY l.security_id ORDER BY l.trade_date
               ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
           ) AS metrics_available_at
    FROM local_metrics l JOIN market m USING (trade_date)
), risk AS (
    SELECT *,
        covar_samp(daily_return, market_return_ew) OVER trailing_60 AS covar,
        var_samp(market_return_ew) OVER trailing_60 AS market_variance,
        var_samp(daily_return) OVER trailing_60 AS return_variance,
        stddev_samp(market_return_ew) OVER trailing_60 AS market_std,
        stddev_samp(daily_return) OVER trailing_60 AS return_std,
        count(daily_return) OVER trailing_60 AS risk_observations
    FROM market_joined
    WINDOW trailing_60 AS (PARTITION BY security_id ORDER BY trade_date ROWS BETWEEN 59 PRECEDING AND CURRENT ROW)
), ranked AS (
    SELECT *,
        CASE WHEN risk_observations = 60 AND market_variance > 0 THEN covar / market_variance END AS beta_60d,
        CASE WHEN risk_observations = 60 AND market_std > 0 AND return_std > 0
             THEN greatest(-1.0, least(1.0, covar / (market_std * return_std))) END AS market_correlation_60d,
        CASE WHEN risk_observations = 60 AND market_variance > 0
             THEN sqrt(greatest(0.0, return_variance - covar * covar / market_variance)) * sqrt(252.0) END AS idiosyncratic_vol_60d
    FROM risk
), final_values AS (
    SELECT *,
        CASE WHEN daily_return IS NOT NULL THEN
            (rank() OVER (PARTITION BY trade_date ORDER BY daily_return)
             + (count(*) OVER (PARTITION BY trade_date, daily_return) - 1) / 2.0)
             / count(daily_return) OVER (PARTITION BY trade_date) END AS daily_return_cs_pct_rank,
        CASE WHEN momentum_21d IS NOT NULL THEN
            (rank() OVER (PARTITION BY trade_date ORDER BY momentum_21d)
             + (count(*) OVER (PARTITION BY trade_date, momentum_21d) - 1) / 2.0)
             / count(momentum_21d) OVER (PARTITION BY trade_date) END AS momentum_21d_cs_pct_rank,
        CASE WHEN realized_vol_20d IS NOT NULL THEN
            (rank() OVER (PARTITION BY trade_date ORDER BY realized_vol_20d)
             + (count(*) OVER (PARTITION BY trade_date, realized_vol_20d) - 1) / 2.0)
             / count(realized_vol_20d) OVER (PARTITION BY trade_date) END AS realized_vol_20d_cs_pct_rank,
        CASE WHEN dollar_volume IS NOT NULL THEN
            (rank() OVER (PARTITION BY trade_date ORDER BY dollar_volume)
             + (count(*) OVER (PARTITION BY trade_date, dollar_volume) - 1) / 2.0)
             / count(dollar_volume) OVER (PARTITION BY trade_date) END AS dollar_volume_cs_pct_rank,
        CASE WHEN amihud_illiquidity_21d IS NOT NULL THEN
            (rank() OVER (PARTITION BY trade_date ORDER BY amihud_illiquidity_21d)
             + (count(*) OVER (PARTITION BY trade_date, amihud_illiquidity_21d) - 1) / 2.0)
             / count(amihud_illiquidity_21d) OVER (PARTITION BY trade_date) END AS amihud_illiquidity_21d_cs_pct_rank
    FROM ranked
)
SELECT
    sha256(concat_ws('|', ?, security_id, cast(trade_date AS VARCHAR))) AS metric_id,
    ? AS source, security_id, symbol, trade_date, numeric_close AS close,
    adj AS adjusted_close, numeric_volume AS volume, dollar_volume, daily_return,
    log_return, gap_return, realized_vol_20d, realized_vol_60d, momentum_21d,
    momentum_126d, CASE WHEN rolling_high_252 > 0 THEN adj / rolling_high_252 - 1.0 END AS pct_from_high_252d,
    avg_dollar_volume_21d, amihud_illiquidity_21d, max_drawdown_126d,
    downside_deviation_60d, market_return_ew, beta_60d, market_correlation_60d,
    idiosyncratic_vol_60d, daily_return_cs_pct_rank, momentum_21d_cs_pct_rank,
    realized_vol_20d_cs_pct_rank, dollar_volume_cs_pct_rank,
    amihud_illiquidity_21d_cs_pct_rank, true AS is_latest_revision,
    trade_date AS as_of_date, metrics_available_at AS available_at, ? AS run_id
FROM final_values
"""

_BULK_SHADOW_TABLE = "equity_price_metrics_bulk_next"
_BULK_STAGE_TABLE = "equity_price_metrics_bulk_stage"
_PHYSICAL_PUBLICATION_COLUMN_NAMES = (
    "metric_id", "source", "security_id", "symbol", "trade_date", "close",
    "adjusted_close", "volume", "dollar_volume", "daily_return", "log_return",
    "gap_return", "realized_vol_20d", "realized_vol_60d", "momentum_21d",
    "momentum_126d", "pct_from_high_252d", "is_latest_revision", "as_of_date",
    "available_at", "run_id", "avg_dollar_volume_21d", "amihud_illiquidity_21d",
    "max_drawdown_126d", "downside_deviation_60d", "market_return_ew", "beta_60d",
    "market_correlation_60d", "idiosyncratic_vol_60d", "daily_return_cs_pct_rank",
    "momentum_21d_cs_pct_rank", "realized_vol_20d_cs_pct_rank",
    "dollar_volume_cs_pct_rank", "amihud_illiquidity_21d_cs_pct_rank",
)
_PUBLICATION_COLUMNS = ", ".join(_PHYSICAL_PUBLICATION_COLUMN_NAMES)
#: Physical column order of the live table since migration 0329 (no PRIMARY KEY, R-4;
#: ``adj_close_basis`` appended, C-61): the shadow must match it for the swap's contract check.
_PHYSICAL_COLUMN_NAMES = (
    *_PHYSICAL_PUBLICATION_COLUMN_NAMES[:21],
    "source_loaded_at", "updated_at",
    *_PHYSICAL_PUBLICATION_COLUMN_NAMES[21:],
    "adj_close_basis",
)
_PHYSICAL_COLUMNS = ", ".join(_PHYSICAL_COLUMN_NAMES)
_STAGE_PHYSICAL_SELECT = ", ".join((
    *_PHYSICAL_PUBLICATION_COLUMN_NAMES[:21],
    "now() AS source_loaded_at", "now() AS updated_at",
    *_PHYSICAL_PUBLICATION_COLUMN_NAMES[21:],
    f"'{ADJ_CLOSE_BASIS}' AS adj_close_basis",
))

_BULK_SHADOW_DDL = f"""
CREATE TABLE {_BULK_SHADOW_TABLE} (
    metric_id VARCHAR NOT NULL,
    source VARCHAR NOT NULL,
    security_id VARCHAR NOT NULL,
    symbol VARCHAR,
    trade_date DATE NOT NULL,
    close DOUBLE,
    adjusted_close DOUBLE,
    volume BIGINT,
    dollar_volume DOUBLE,
    daily_return DOUBLE,
    log_return DOUBLE,
    gap_return DOUBLE,
    realized_vol_20d DOUBLE,
    realized_vol_60d DOUBLE,
    momentum_21d DOUBLE,
    momentum_126d DOUBLE,
    pct_from_high_252d DOUBLE,
    is_latest_revision BOOLEAN NOT NULL DEFAULT true,
    as_of_date DATE NOT NULL,
    available_at TIMESTAMP NOT NULL,
    run_id VARCHAR,
    source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
    updated_at TIMESTAMP NOT NULL DEFAULT now(),
    avg_dollar_volume_21d DOUBLE,
    amihud_illiquidity_21d DOUBLE,
    max_drawdown_126d DOUBLE,
    downside_deviation_60d DOUBLE,
    market_return_ew DOUBLE,
    beta_60d DOUBLE,
    market_correlation_60d DOUBLE,
    idiosyncratic_vol_60d DOUBLE,
    daily_return_cs_pct_rank DOUBLE,
    momentum_21d_cs_pct_rank DOUBLE,
    realized_vol_20d_cs_pct_rank DOUBLE,
    dollar_volume_cs_pct_rank DOUBLE,
    amihud_illiquidity_21d_cs_pct_rank DOUBLE,
    adj_close_basis VARCHAR
)
"""


def _metric_id_prefix_bounds(prefix: int) -> tuple[str, str | None]:
    """Contiguous SHA-256 key intervals; 256 small batches bound PK ART state."""
    lower = f"{prefix:02x}"
    return lower, None if prefix == 255 else f"{prefix + 1:02x}"


def _build_publication_shadow(store: DuckDBStore, source: str) -> int:
    """Build the PK-only publication shadow in ordered, independently committed chunks."""
    store.con.execute(f"DROP TABLE IF EXISTS {_BULK_SHADOW_TABLE}")
    store.con.execute(_BULK_SHADOW_DDL)
    for prefix in range(256):
        lower, upper = _metric_id_prefix_bounds(prefix)
        upper_clause = "AND metric_id < ?" if upper is not None else ""
        store.con.execute(f"""
            INSERT INTO {_BULK_SHADOW_TABLE} ({_PHYSICAL_COLUMNS})
            SELECT * FROM (
                SELECT {_PHYSICAL_COLUMNS}
                FROM equity_price_metrics
                WHERE source <> ? AND metric_id >= ? {upper_clause}
                UNION ALL
                SELECT {_STAGE_PHYSICAL_SELECT}
                FROM {_BULK_STAGE_TABLE}
                WHERE metric_id >= ? {upper_clause}
            ) ordered_rows
            ORDER BY metric_id
        """, [source, lower, *(() if upper is None else (upper,)), lower,
               *(() if upper is None else (upper,))])
        if prefix % 16 == 15 and prefix != 255:
            store.close()
            store.reopen()
    row = store.con.execute(f"SELECT count(*) FROM {_BULK_SHADOW_TABLE}").fetchone()
    return 0 if row is None else int(row[0])


def refresh_equity_price_metrics(store: DuckDBStore, options: EquityPriceMetricsOptions) -> int:
    """Bounded SQL refresh with disk spill; never loads the bar universe into pandas.

    The picked, artifact-repaired bars are materialized first (:func:`_bars_sql`), then the
    metrics stage reads them. A persistent stage and a PK-only shadow are built before the short atomic table
    swap.  Hash-prefix inserts and checkpoint/reopen cycles bound primary-key index
    work; a failed build leaves the preceding live table untouched.  Production
    callers must keep DuckDB at one thread and 1GB.
    """
    store.initialize()
    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    registered = False
    symbol_pred = "TRUE"
    if options.symbols is not None and not symbols:
        symbol_pred = "FALSE"
    elif symbols:
        store.con.register("eqpm_refresh_symbols", pd.DataFrame({"symbol": sorted({str(s).strip().upper() for s in symbols})}))
        registered = True
        symbol_pred = "b.symbol IN (SELECT symbol FROM eqpm_refresh_symbols)"
    cutoff = options.as_of_date
    cutoff_at = None if cutoff is None else dt.datetime.combine(cutoff, dt.time.max)
    try:
        store.con.execute(_bars_sql(symbol_pred), [cutoff, cutoff, cutoff_at, cutoff_at])
        store.con.execute(_REFRESH_SQL, [options.source, options.source, options.run_id])
        store.con.execute(f"DROP TABLE {_BARS_STAGE_TABLE}")
        row = store.con.execute(f"SELECT count(*) FROM {_BULK_STAGE_TABLE}").fetchone()
        rows = 0 if row is None else int(row[0])
        shadow_rows = _build_publication_shadow(store, options.source)
        expected = store.con.execute("""
            SELECT (SELECT count(*) FROM equity_price_metrics WHERE source <> ?)
                 + (SELECT count(*) FROM equity_price_metrics_bulk_stage)
        """, [options.source]).fetchone()
        if expected is None or shadow_rows != int(expected[0]):
            raise RuntimeError("equity price metrics shadow row count does not match publication inputs")
        publish_validated_shadow(
            store,
            live_table="equity_price_metrics",
            shadow_table=_BULK_SHADOW_TABLE,
        )
        store.con.execute(f"DROP TABLE {_BULK_STAGE_TABLE}")
        return rows
    finally:
        if registered:
            store.con.unregister("eqpm_refresh_symbols")


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
            details={"source": options.source, "adj_close_basis": ADJ_CLOSE_BASIS},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"grain": "security_id,trade_date", "adj_close_basis": ADJ_CLOSE_BASIS},
        )
