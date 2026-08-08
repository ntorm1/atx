"""Tests for plot_var_cumulative_pnl.py's P&L decomposition.

Focus: ``compute_decomposition`` splits the cumulative restruck-scenario P&L
trace into (1) ``rebasing_reset``, the value jump between the one-session-aged
book and the freshly restruck book on the same date, and (2)
``cumulative_held_drift``, the telescoped revaluation drift of holding the
restruck-once profile across each chain of adjacent scenarios. See
docs/historical-var-engine-status.md and the Task 1 forensic decomposition for
why cumulating raw scenario P&L overstates held-book drift by re-basing resets.
"""

import math
from datetime import date

import numpy as np
import pandas as pd
import pytest

from plot_var_cumulative_pnl import compute_decomposition


def _synthetic_frame() -> pd.DataFrame:
    """Four scenarios: rows 0-1 chained, a history break, rows 2-3 chained.

    Hand-computed expectations (see module docstring for the definitions):

    rebasing_reset[0] = base_value[1] - shifted_value[0] = -950 - (-900) = -50
    rebasing_reset[1] = NaN (shifted_date[1] 2026-01-06 != base_date[2] 2026-01-12)
    rebasing_reset[2] = base_value[3] - shifted_value[2] = -680 - (-650) = -30
    rebasing_reset[3] = NaN (no row 4 to reset against)

    cumulative_held_drift[0] = 100 + 0                    = 100
    cumulative_held_drift[1] = 250 + (-50)                = 200
    cumulative_held_drift[2] = 300 + (-50)                = 250
    cumulative_held_drift[3] = 380 + (-50 + -30)          = 300
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


def test_rebasing_reset_matches_hand_computed_values_on_chained_rows():
    result = compute_decomposition(_synthetic_frame())

    assert result["rebasing_reset"].iloc[0] == pytest.approx(-50.0)
    assert result["rebasing_reset"].iloc[2] == pytest.approx(-30.0)


def test_rebasing_reset_is_nan_at_history_break_and_trailing_row():
    result = compute_decomposition(_synthetic_frame())

    # Row 1 -> row 2 is a history break: shifted_date[1] != base_date[2].
    assert math.isnan(result["rebasing_reset"].iloc[1])
    # The last row has no successor to reset against.
    assert math.isnan(result["rebasing_reset"].iloc[3])


def test_cumulative_held_drift_matches_hand_computed_values():
    result = compute_decomposition(_synthetic_frame())

    expected = [100.0, 200.0, 250.0, 300.0]
    assert result["cumulative_held_drift"].tolist() == pytest.approx(expected)


def test_cumulative_held_drift_identity_at_last_row():
    """cumulative_held_drift[last] == cumulative_pnl[last] + nansum(rebasing_reset[:-1])."""
    result = compute_decomposition(_synthetic_frame())

    reset = result["rebasing_reset"].to_numpy()
    expected_last = result["cumulative_pnl"].iloc[-1] + np.nansum(reset[:-1])
    assert expected_last == pytest.approx(300.0)
    assert result["cumulative_held_drift"].iloc[-1] == pytest.approx(expected_last)


def test_compute_decomposition_does_not_mutate_input():
    frame = _synthetic_frame()
    snapshot = frame.copy(deep=True)

    compute_decomposition(frame)

    pd.testing.assert_frame_equal(frame, snapshot)
