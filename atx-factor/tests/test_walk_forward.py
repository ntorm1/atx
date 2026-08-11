from __future__ import annotations

import datetime as dt

import numpy as np
import polars as pl

from atx_factor.config import BacktestConfig, CostModel, PortfolioConfig, WalkForwardConfig
from atx_factor.walk_forward import walk_forward_backtest


def _monthly_dates(count: int) -> list[dt.date]:
    dates: list[dt.date] = []
    year, month = 2015, 1
    for _ in range(count):
        dates.append(dt.date(year, month, 15))
        month += 1
        if month == 13:
            year += 1
            month = 1
    return dates


def _panel() -> pl.DataFrame:
    rng = np.random.default_rng(7)
    rows: list[dict[str, object]] = []
    for date in _monthly_dates(84):
        signal = rng.normal(size=30)
        returns = 0.015 * signal + rng.normal(scale=0.01, size=30)
        for index in range(30):
            rows.append(
                {
                    "date": date,
                    "asset_id": f"S{index:03d}",
                    "signal": float(signal[index]),
                    "forward_return": float(returns[index]),
                    "adv_usd": 1_000_000_000.0,
                }
            )
    return pl.DataFrame(rows)


def test_walk_forward_produces_disjoint_chronological_oos_folds() -> None:
    result = walk_forward_backtest(
        _panel(),
        BacktestConfig(
            portfolio=PortfolioConfig(minimum_names=20, name_cap=0.05),
            costs=CostModel(impact_bps=0.0, annual_borrow_bps=0.0),
        ),
        WalkForwardConfig(
            minimum_train_periods=36,
            test_periods=12,
            step_periods=12,
            embargo_periods=1,
            minimum_folds=3,
        ),
    )
    assert result.folds.height == 3
    assert result.periods.height == 36
    assert result.periods.get_column("date").n_unique() == 36
    assert result.metrics.sharpe > 0

