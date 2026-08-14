from __future__ import annotations

import datetime as dt

import polars as pl

from atx_factor.capacity import evaluate_capacity_frontier
from atx_factor.config import CostModel, PortfolioConfig


def _panel() -> pl.DataFrame:
    rows: list[dict[str, object]] = []
    for month in range(1, 5):
        date = dt.date(2025, month, 28)
        for index in range(20):
            rows.append(
                {
                    "date": date,
                    "asset_id": f"A{index:02d}",
                    "signal": float(index),
                    "forward_return": (index - 9.5) / 10_000.0,
                    "adv_usd": 100_000_000.0,
                }
            )
    return pl.DataFrame(rows)


def test_capacity_frontier_reports_maximum_feasible_grid_point() -> None:
    result = evaluate_capacity_frontier(
        _panel(),
        [1_000_000.0, 10_000_000.0, 100_000_000.0],
        portfolio=PortfolioConfig(minimum_names=20, name_cap=0.10),
        costs=CostModel(
            commission_bps=0.0,
            half_spread_bps=0.0,
            impact_bps=0.0,
            annual_borrow_bps=0.0,
        ),
    )
    assert result.maximum_feasible_aum_usd == 10_000_000.0
    assert [point.feasible for point in result.points] == [True, True, False]
    assert result.points[-1].minimum_gross_deployment < 0.95
    assert result.to_dict()["maximum_feasible_aum_usd"] == 10_000_000.0
