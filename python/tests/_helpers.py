"""Shared builders for backtest tests (importable by bare name under pytest)."""

from atxpy import _core


def one_symbol_params(n=5):
    """A minimal single-name, always-long backtest with `n` daily bars."""
    p = _core.BacktestParams()
    p.symbols = ["AAA"]
    bars = _core.BarsForSymbol()
    bars.ts_nanos = [int(i) * 86_400_000_000_000 for i in range(n)]
    bars.open = [100.0] * n
    bars.high = [101.0] * n
    bars.low = [99.0] * n
    bars.close = [100.0 + i for i in range(n)]
    bars.volume = [1_000_000.0] * n
    p.bars = [bars]
    p.starting_cash = "1000000.0"
    p.signals = [[1.0]] * n
    p.max_lookback = 1
    p.every = 1
    return p


def two_symbol_params(n=5):
    """A long/short two-name backtest — cross-sectional weights are non-zero, so
    it actually trades (a single name is forced flat by dollar-neutral ranking)."""
    p = _core.BacktestParams()
    syms = ["AAA", "BBB"]
    p.symbols = syms
    bars_list = []
    for s_idx in range(len(syms)):
        bars = _core.BarsForSymbol()
        bars.ts_nanos = [int(i) * 86_400_000_000_000 for i in range(n)]
        bars.open = [100.0] * n
        bars.high = [101.0] * n
        bars.low = [99.0] * n
        bars.close = [100.0 + i + s_idx for i in range(n)]
        bars.volume = [1_000_000.0] * n
        bars_list.append(bars)
    p.bars = bars_list
    p.starting_cash = "1000000.0"
    p.signals = [[1.0, -1.0]] * n  # long AAA / short BBB each rebalance
    p.max_lookback = 1
    p.every = 1
    return p
