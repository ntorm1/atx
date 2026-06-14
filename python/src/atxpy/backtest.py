"""Pythonic facade over atxpy._core.run_backtest (pandas in / pandas out)."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

import numpy as np

from . import _core


@dataclass
class BacktestResult:
    """Result of a backtest, with pandas views over the engine output."""

    equity_curve: Any  # pd.DataFrame [timestamp, equity, gross, net]
    fills: Any         # pd.DataFrame [symbol, qty, price, fee, impact, timestamp]
    final_cash: _core.Decimal
    final_equity: float
    turnover: float
    slices: int
    rebalances: int


def _require_pandas():
    try:
        import pandas as pd
    except ImportError as exc:  # pragma: no cover
        raise ImportError("atxpy.run_backtest needs pandas: pip install atxpy[pandas]") from exc
    return pd


def run_backtest(bars, signals, *, symbols=None, starting_cash="1000000.0",
                 max_lookback=1, every=1, delay_same=False, policy=None,
                 fill=None, slip=None, impact=None, comm=None, latency=None,
                 volcap=None, stats=None):
    """Run a backtest from a long-format bars DataFrame and a signals matrix.

    bars: DataFrame with columns [symbol, timestamp, open, high, low, close, volume];
          timestamp is int64 unix-nanos. Rows are grouped by symbol and sorted by
          timestamp to form the per-symbol feed.
    signals: 2D array-like [n_rebalances, n_symbols] of alpha scores, column-aligned
             to `symbols` (NaN = no opinion). Row k drives the k-th rebalance.
    symbols: explicit universe order; defaults to sorted(unique(bars.symbol)).
    """
    pd = _require_pandas()
    if symbols is None:
        symbols = sorted(bars["symbol"].unique())
    symbols = list(symbols)

    p = _core.BacktestParams()
    p.symbols = symbols
    bars_list = []
    for sym in symbols:
        g = bars[bars["symbol"] == sym].sort_values("timestamp")
        b = _core.BarsForSymbol()
        b.ts_nanos = g["timestamp"].to_numpy(dtype=np.int64).tolist()
        b.open = g["open"].to_numpy(dtype=np.float64).tolist()
        b.high = g["high"].to_numpy(dtype=np.float64).tolist()
        b.low = g["low"].to_numpy(dtype=np.float64).tolist()
        b.close = g["close"].to_numpy(dtype=np.float64).tolist()
        b.volume = g["volume"].to_numpy(dtype=np.float64).tolist()
        bars_list.append(b)
    p.bars = bars_list
    p.starting_cash = str(starting_cash)

    sig = np.asarray(signals, dtype=np.float64)
    if sig.ndim != 2:
        raise ValueError("signals must be 2D [n_rebalances, n_symbols]")
    if sig.shape[1] != len(symbols):
        raise ValueError(f"signals has {sig.shape[1]} cols, expected {len(symbols)}")
    p.signals = [row.tolist() for row in sig]

    p.max_lookback = int(max_lookback)
    p.every = int(every)
    p.delay_same = bool(delay_same)
    if policy is not None:
        p.policy = policy
    for name, cfg in dict(fill=fill, slip=slip, impact=impact, comm=comm,
                          latency=latency, volcap=volcap).items():
        if cfg is not None:
            setattr(p, name, cfg)
    if stats is not None:
        p.stats = stats

    r = _core.run_backtest(p)

    ts, eq, gross, net = r.equity_columns()
    equity_curve = pd.DataFrame({"timestamp": ts, "equity": eq, "gross": gross, "net": net})
    fid, fqty, fprice, ffee, fimpact, ft = r.fills_columns()
    name_by_id = {i: s for i, s in enumerate(symbols)}
    fills = pd.DataFrame({
        "symbol": [name_by_id.get(int(i), int(i)) for i in fid],
        "qty": fqty, "price": fprice, "fee": ffee, "impact": fimpact, "timestamp": ft,
    })
    return BacktestResult(
        equity_curve=equity_curve, fills=fills, final_cash=r.final_cash,
        final_equity=r.final_equity, turnover=r.turnover, slices=r.slices,
        rebalances=r.rebalances,
    )
