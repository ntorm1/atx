from __future__ import annotations

import datetime as dt

import polars as pl
import pytest

from atx_factor.backtest import run_backtest
from atx_factor.config import BacktestConfig, CostModel, PortfolioConfig


def _panel() -> pl.DataFrame:
    rows: list[dict[str, object]] = []
    for date, end_date in (
        (dt.date(2025, 1, 31), dt.date(2025, 3, 3)),
        (dt.date(2025, 2, 28), dt.date(2025, 3, 31)),
    ):
        for asset_id, signal, forward_return in (
            ("A", -2.0, -0.01),
            ("B", -1.0, -0.01),
            ("C", 1.0, 0.01),
            ("D", 2.0, 0.01),
        ):
            rows.append(
                {
                    "date": date,
                    "asset_id": asset_id,
                    "signal": signal,
                    "forward_return": forward_return,
                    "forward_end_date": end_date,
                    "adv_usd": 1_000_000_000.0,
                }
            )
    return pl.DataFrame(rows)


def _zero_cost_config() -> BacktestConfig:
    return BacktestConfig(
        portfolio=PortfolioConfig(minimum_names=4, name_cap=0.25),
        costs=CostModel(
            commission_bps=0.0,
            half_spread_bps=0.0,
            impact_bps=0.0,
            annual_borrow_bps=0.0,
        ),
    )


def test_vectorized_backtest_pnl_turnover_and_equity() -> None:
    result = run_backtest(_panel(), _zero_cost_config())
    periods = result.periods
    assert periods.get_column("gross_return").to_list() == pytest.approx([0.01, 0.01])
    assert periods.get_column("net_return").to_list() == pytest.approx([0.01, 0.01])
    assert periods.get_column("turnover").to_list() == pytest.approx([0.5, 0.0])
    assert periods.get_column("equity").to_list() == pytest.approx([1.01, 1.0201])
    assert result.metrics.total_return == pytest.approx(0.0201)


def test_costs_reduce_net_return_and_are_deterministic() -> None:
    config = BacktestConfig(
        portfolio=PortfolioConfig(minimum_names=4, name_cap=0.25),
        costs=CostModel(
            commission_bps=1.0,
            half_spread_bps=2.0,
            impact_bps=5.0,
            annual_borrow_bps=100.0,
        ),
        trial_count=10,
    )
    first = run_backtest(_panel(), config)
    second = run_backtest(_panel().reverse(), config)
    assert first.periods.equals(second.periods)
    assert first.periods.get_column("total_cost").sum() > 0
    assert first.periods.get_column("net_return").sum() < 0.02

