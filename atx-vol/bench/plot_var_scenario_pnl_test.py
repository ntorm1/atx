"""Tests for the one-day historical VaR report helpers."""

from datetime import date

import pytest

from plot_var_scenario_pnl import (
    PnlRow,
    count_history_breaks,
    historical_loss_statistics,
)


def _rows(pnl: list[float]) -> list[PnlRow]:
    dates = [
        (date(2026, 1, 2), date(2026, 1, 5)),
        (date(2026, 1, 5), date(2026, 1, 6)),
        (date(2026, 1, 12), date(2026, 1, 13)),
        (date(2026, 1, 13), date(2026, 1, 14)),
        (date(2026, 1, 14), date(2026, 1, 15)),
    ]
    result: list[PnlRow] = []
    for index, value in enumerate(pnl):
        base_date, shifted_date = dates[index]
        result.append(
            PnlRow(
                base_date,
                shifted_date,
                1000.0,
                1000.0 + value,
                value,
                1,
                1,
                0,
                0,
                0,
                0,
            )
        )
    return result


def test_historical_loss_statistics_matches_nearest_rank_and_inclusive_tail():
    # P&Ls [100, -50, -200, 25, -75] -> sorted losses [-100, -25, 50, 75, 200].
    # ceil(.80 * 5) - 1 = 3, so VaR=75 and ES=(75+200)/2.
    stats = historical_loss_statistics(_rows([100.0, -50.0, -200.0, 25.0, -75.0]), 0.80)

    assert stats.value_at_risk == pytest.approx(75.0)
    assert stats.expected_shortfall == pytest.approx(137.5)
    assert stats.n_scenarios == 5


@pytest.mark.parametrize("confidence", [0.0, 1.0, -0.1, 1.1])
def test_historical_loss_statistics_rejects_invalid_confidence(confidence: float):
    with pytest.raises(ValueError, match="confidence"):
        historical_loss_statistics(_rows([1.0]), confidence)


def test_count_history_breaks_uses_shifted_to_next_base_adjacency():
    assert count_history_breaks(_rows([1.0, 2.0, 3.0, 4.0])) == 1
