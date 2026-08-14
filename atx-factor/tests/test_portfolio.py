from __future__ import annotations

import datetime as dt

import polars as pl
import pytest

from atx_factor.config import PortfolioConfig
from atx_factor.portfolio import _native_waterfill, build_target_weights


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


def test_waterfill_handles_target_equal_to_capacity_with_roundoff() -> None:
    frame = pl.DataFrame(
        {
            "date": [dt.date(2025, 1, 31)] * 2,
            "asset_id": ["A", "B"],
            "magnitude": [1.0, 2.0],
            "cap": [0.1, 0.2],
            "target": [0.30000000000000004] * 2,
        }
    )
    result = _native_waterfill(
        frame,
        magnitude_column="magnitude",
        output_column="weight",
        target_column="target",
        cap_column="cap",
    ).sort("asset_id")
    assert result.get_column("weight").to_list() == pytest.approx([0.1, 0.2])


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


def test_tail_book_keeps_only_predeclared_extremes() -> None:
    names = 20
    panel = pl.DataFrame(
        {
            "date": [dt.date(2025, 1, 31)] * names,
            "asset_id": [f"A{index:02d}" for index in range(names)],
            "signal": list(range(names)),
            "forward_return": [0.0] * names,
        }
    )
    result = build_target_weights(
        panel,
        PortfolioConfig(
            minimum_names=names,
            name_cap=0.25,
            tail_fraction=0.20,
        ),
    )
    nonzero = result.filter(pl.col("target_weight") != 0)
    assert nonzero.height == 8
    assert nonzero.get_column("asset_id").to_list() == [
        "A00",
        "A01",
        "A02",
        "A03",
        "A16",
        "A17",
        "A18",
        "A19",
    ]
    assert result.get_column("target_weight").sum() == pytest.approx(0.0)
    assert result.get_column("target_weight").abs().sum() == pytest.approx(1.0)


@pytest.mark.parametrize(
    ("construction", "tail_sign"),
    [
        ("long_tail_vs_universe", 1),
        ("universe_vs_short_tail", -1),
    ],
)
def test_asymmetric_tail_books_isolate_one_extreme_against_broad_universe(
    construction: str,
    tail_sign: int,
) -> None:
    names = 20
    panel = pl.DataFrame(
        {
            "date": [dt.date(2025, 1, 31)] * names,
            "asset_id": [f"A{index:02d}" for index in range(names)],
            "signal": list(range(names)),
            "forward_return": [0.0] * names,
        }
    )
    result = build_target_weights(
        panel,
        PortfolioConfig(
            minimum_names=names,
            name_cap=0.25,
            tail_fraction=0.20,
            construction=construction,
        ),
    )
    weights = result.get_column("target_weight")
    tail = weights.tail(4) if tail_sign > 0 else weights.head(4)
    broad = weights.head(16) if tail_sign > 0 else weights.tail(16)
    assert (tail.sign() == tail_sign).all()
    assert (broad.sign() == -tail_sign).all()
    assert tail.sum() == pytest.approx(0.5 * tail_sign)
    assert broad.sum() == pytest.approx(-0.5 * tail_sign)
    assert weights.abs().sum() == pytest.approx(1.0)
