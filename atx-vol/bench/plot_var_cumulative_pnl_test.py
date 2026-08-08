"""Tests for plot_var_cumulative_pnl.py's P&L decomposition.

Focus: ``compute_decomposition`` splits the cumulative restruck-scenario P&L
trace into (1) ``rebasing_reset``, the value jump between the one-session-aged
book and the freshly restruck book (computed for every adjacent pair, chained
or not — a history break conflates the break's own market move with the
re-basing jump, an accepted convention), and (2) ``cumulative_held_drift``,
the telescoped revaluation drift of holding a restruck-once profile from the
first base date onward. See docs/historical-var-engine-status.md and the
Task 1 forensic decomposition for why cumulating raw scenario P&L overstates
held-book drift by re-basing resets.
"""

from datetime import date

import math

import pandas as pd
import pytest

from plot_var_cumulative_pnl import compute_decomposition


def _synthetic_frame() -> pd.DataFrame:
    """Four scenarios: rows 0-1 chained, a history break, rows 2-3 chained.

    Hand-computed expectations (see module docstring for the definitions):

    rebasing_reset[0] = base_value[1] - shifted_value[0] = -950 - (-900) = -50   (chained)
    rebasing_reset[1] = base_value[2] - shifted_value[1] = -700 - (-800) = 100   (break)
    rebasing_reset[2] = base_value[3] - shifted_value[2] = -680 - (-650) = -30   (chained)
    rebasing_reset[3] = NaN (no row 4 to reset against)

    chained[0] = True  (shifted_date[0] 2026-01-05 == base_date[1] 2026-01-05)
    chained[1] = False (shifted_date[1] 2026-01-06 != base_date[2] 2026-01-12)
    chained[2] = True  (shifted_date[2] 2026-01-13 == base_date[3] 2026-01-13)
    chained[3] = NA    (no row 4 to compare against)

    cumulative_held_drift[k] = cumulative_pnl[k] + sum(rebasing_reset[i] for i < k):
    cumulative_held_drift[0] = 100 + 0                     = 100
    cumulative_held_drift[1] = 250 + (-50)                 = 200
    cumulative_held_drift[2] = 300 + (-50 + 100)           = 350
    cumulative_held_drift[3] = 380 + (-50 + 100 + -30)     = 400
    """
    return pd.DataFrame(
        {
            "base_date": [
                date(2026, 1, 2),
                date(2026, 1, 5),
                date(2026, 1, 12),
                date(2026, 1, 13),
            ],
            "shifted_date": [
                date(2026, 1, 5),
                date(2026, 1, 6),
                date(2026, 1, 13),
                date(2026, 1, 14),
            ],
            "base_value": [-1000.0, -950.0, -700.0, -680.0],
            "shifted_value": [-900.0, -800.0, -650.0, -600.0],
            "pnl": [100.0, 150.0, 50.0, 80.0],
            "cumulative_pnl": [100.0, 250.0, 300.0, 380.0],
        }
    )


def test_rebasing_reset_matches_hand_computed_values_on_every_adjacent_pair():
    """A history-break pair now gets a real numeric reset too, not NaN."""
    result = compute_decomposition(_synthetic_frame())

    assert result["rebasing_reset"].iloc[0] == pytest.approx(-50.0)  # chained
    assert result["rebasing_reset"].iloc[1] == pytest.approx(100.0)  # break
    assert result["rebasing_reset"].iloc[2] == pytest.approx(-30.0)  # chained


def test_rebasing_reset_is_nan_only_at_trailing_row():
    result = compute_decomposition(_synthetic_frame())

    assert math.isnan(result["rebasing_reset"].iloc[3])


def test_chained_flag_matches_date_adjacency_and_is_na_at_trailing_row():
    result = compute_decomposition(_synthetic_frame())

    assert bool(result["chained"].iloc[0]) is True
    assert bool(result["chained"].iloc[1]) is False
    assert bool(result["chained"].iloc[2]) is True
    assert pd.isna(result["chained"].iloc[3])


def test_cumulative_held_drift_matches_hand_computed_values():
    result = compute_decomposition(_synthetic_frame())

    expected = [100.0, 200.0, 350.0, 400.0]
    assert result["cumulative_held_drift"].tolist() == pytest.approx(expected)


def test_cumulative_held_drift_identity_via_cumulative_pnl_and_resets():
    """cumulative_held_drift[last] == cumulative_pnl[last] + sum(rebasing_reset[:-1])."""
    result = compute_decomposition(_synthetic_frame())

    reset = result["rebasing_reset"].to_numpy()
    expected_last = result["cumulative_pnl"].iloc[-1] + reset[:-1].sum()
    assert expected_last == pytest.approx(400.0)
    assert result["cumulative_held_drift"].iloc[-1] == pytest.approx(expected_last)


def test_cumulative_held_drift_identity_via_base_value_telescope():
    """base_value[i+1] - base_value[i] == pnl[i] + rebasing_reset[i] for every pair,
    so cumulative_held_drift[last] == (base_value[last] + pnl[last]) - base_value[0].
    """
    result = compute_decomposition(_synthetic_frame())

    expected_last = (
        result["base_value"].iloc[-1] + result["pnl"].iloc[-1] - result["base_value"].iloc[0]
    )
    assert expected_last == pytest.approx(400.0)
    assert result["cumulative_held_drift"].iloc[-1] == pytest.approx(expected_last)


def test_compute_decomposition_does_not_mutate_input():
    frame = _synthetic_frame()
    snapshot = frame.copy(deep=True)

    compute_decomposition(frame)

    pd.testing.assert_frame_equal(frame, snapshot)
