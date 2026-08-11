from __future__ import annotations

import datetime as dt

import polars as pl
import pytest

from atx_factor.config import PortfolioConfig
from atx_factor.portfolio import build_target_weights


def _cross_section() -> pl.DataFrame:
    return pl.DataFrame(
        {
            "date": [dt.date(2025, 1, 31)] * 4,
            "asset_id": ["A", "B", "C", "D"],
            "signal": [-2.0, -1.0, 1.0, 2.0],
            "forward_return": [0.0] * 4,
        }
    )


def test_rank_book_is_exactly_neutral_gross_scaled_capped_and_deterministic() -> None:
    config = PortfolioConfig(minimum_names=4, name_cap=0.25)
    first = build_target_weights(_cross_section(), config)
    second = build_target_weights(_cross_section().reverse(), config)
    assert first.equals(second)
    assert first.get_column("target_weight").to_list() == pytest.approx(
        [-0.25, -0.25, 0.25, 0.25]
    )
    assert first.get_column("target_weight").sum() == pytest.approx(0.0)
    assert first.get_column("target_weight").abs().sum() == pytest.approx(1.0)


def test_name_cap_underdeploys_neutrally_when_full_gross_is_infeasible() -> None:
    result = build_target_weights(
        _cross_section(),
        PortfolioConfig(minimum_names=4, name_cap=0.20),
    )
    assert result.get_column("target_weight").sum() == pytest.approx(0.0)
    assert result.get_column("target_weight").abs().sum() == pytest.approx(0.8)
    assert result.get_column("capacity_scale").unique().to_list() == pytest.approx([0.8])


def test_odd_rank_cross_section_drops_centering_residue_before_capacity_solve() -> None:
    names = 77
    panel = pl.DataFrame(
        {
            "date": [dt.date(2025, 1, 31)] * names,
            "asset_id": [f"A{index:03d}" for index in range(names)],
            "signal": list(range(names)),
            "forward_return": [0.0] * names,
        }
    )
    result = build_target_weights(
        panel,
        PortfolioConfig(minimum_names=names, name_cap=0.01),
    )
    weights = result.get_column("target_weight")
    assert weights.sum() == pytest.approx(0.0, abs=1e-12)
    assert weights.abs().sum() == pytest.approx(0.76)
    assert (weights != 0).sum() == 76


def test_flat_cross_section_produces_flat_book() -> None:
    panel = pl.DataFrame(
        {
            "date": [dt.date(2025, 1, 31)] * 20,
            "asset_id": [f"A{index:03d}" for index in range(20)],
            "signal": [1.0] * 20,
            "forward_return": [0.0] * 20,
        }
    )
    result = build_target_weights(panel, PortfolioConfig(minimum_names=20))
    assert result.get_column("target_weight").abs().sum() == pytest.approx(0.0)
    assert result.get_column("capacity_scale").unique().to_list() == [0.0]
