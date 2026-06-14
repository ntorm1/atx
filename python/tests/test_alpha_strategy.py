from atxpy import _core


def _two_name_bars(n=8):
    p = _core.BacktestParams()
    p.symbols = ["AAA", "BBB"]
    bars_list = []
    for s in range(2):
        b = _core.BarsForSymbol()
        b.ts_nanos = [i * 86_400_000_000_000 for i in range(n)]
        b.open = [100.0] * n
        b.high = [101.0] * n
        b.low = [99.0] * n
        # AAA trends up, BBB trends down -> momentum goes long AAA / short BBB
        b.close = [100.0 + (i if s == 0 else -i) for i in range(n)]
        b.volume = [1_000_000.0] * n
        bars_list.append(b)
    p.bars = bars_list
    p.starting_cash = "1000000.0"
    p.every = 1
    return p


def test_alpha_strategy_runs():
    p = _two_name_bars(8)
    p.alpha_expr = "rank(delta(close, 1))"  # cross-sectional momentum
    r = _core.run_backtest(p)
    assert r.slices == 8
    _, fqty, *_ = r.fills_columns()
    assert fqty.size >= 1  # the DSL strategy actually traded


def test_alpha_strategy_deterministic():
    p = _two_name_bars(8)
    p.alpha_expr = "rank(delta(close, 1))"
    a = _core.run_backtest(p)
    b = _core.run_backtest(p)
    assert a.final_cash.raw() == b.final_cash.raw()
