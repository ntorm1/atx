"""Tier1-S4 T2 fix round 1: the interval-close under-extension bug (review High #1).

Isolated in its own file per the fix-round file fence: ``tests/test_universe_us_listed.py``
is being edited concurrently by the Task 1 fix round, so this file owns its own minimal,
self-contained fixtures rather than importing private helpers from that file.

The bug: ``compute_universe_us_listed_intervals``'s ``close()`` helper used to take a
binary ``extend: bool``, extending the full lookback window only when the close was
triggered by a real session-rank gap (or the security's final decision) and applying ZERO
extension whenever the close was triggered by a same-grid state change (a different
``security_type``/``exchange_code``/``has_cik``/``reason``) with any smaller gap. That
under-extended: a security that traded at rank 1 and again at rank 4 (gap 3, well inside a
5-session lookback) never actually lost membership under rule 1 of the membership contract
("at least one trade in the prior N trading days"), but the old code reported a 2-session
hole between the two intervals.

The fix bounds the extension at ``min(final_rank + lookback_days - 1, next_rank - 1)``
when a successor decision is known (state change or real gap), so a closing interval is
always extended right up to (but never past) the successor's ``valid_from`` -- the two
intervals abut with zero dropped days whenever the gap to the next decision is within the
lookback window, and a genuine hole only appears when it exceeds it.
"""

from __future__ import annotations

import datetime as dt

import pandas as pd

from atx_db.universe_us_listed import (
    UniverseUsListedOptions,
    compute_universe_us_listed_intervals,
)

SESSION_DATES = [
    "2024-02-01",
    "2024-02-02",
    "2024-02-03",
    "2024-02-04",
    "2024-02-05",
    "2024-02-06",
    "2024-02-07",
    "2024-02-08",
    "2024-02-09",
    "2024-02-10",
    "2024-02-11",
    "2024-02-12",
]


def _sessions(dates=SESSION_DATES):
    return pd.DataFrame(
        {
            "trade_date": [dt.date.fromisoformat(d) for d in dates],
            "session_rank": range(1, len(dates) + 1),
        }
    )


def _rank_date(rank: int) -> dt.date:
    return dt.date.fromisoformat(SESSION_DATES[rank - 1])


def _decision(security_id, rank, **overrides):
    date = SESSION_DATES[rank - 1]
    row = {
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date.fromisoformat(date),
        "session_rank": rank,
        "available_at": pd.Timestamp(f"{date} 22:00:00"),
        "security_type": "common",
        "exchange_code": "XNAS",
        "has_cik": True,
        "cik": "0000320193",
        "market_cap_decile": 9,
    }
    row.update(overrides)
    return row


def _options(**overrides):
    return UniverseUsListedOptions(run_id="test-run", **overrides)


def test_state_change_within_lookback_bridges_with_no_dropped_days():
    """Reviewer's repro: lookback=5, decisions at rank 1 and rank 4, exchange changes."""

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", 1, exchange_code="XNYS"),
            _decision("SEC-1", 4, exchange_code="XNAS"),
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(), _options(lookback_days=5))
    assert len(out) == 2
    first, second = out.iloc[0], out.iloc[1]
    assert first["exchange_code"] == "XNYS"
    assert second["exchange_code"] == "XNAS"
    assert first["valid_from"] == _rank_date(1)
    # Bounded at next_rank - 1 = 3, not the full extension (which would reach rank 5).
    assert first["valid_to"] == _rank_date(3)
    assert second["valid_from"] == _rank_date(4)
    # No dropped day: the closing interval's valid_to is the session immediately before
    # the successor's valid_from.
    assert first["valid_to"] == second["valid_from"] - dt.timedelta(days=1)


def test_gap_equal_to_lookback_with_state_change_extends_to_the_exact_boundary():
    """gap == lookback_days with a state change: extension and bound coincide exactly."""

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", 1, exchange_code="XNYS"),
            _decision("SEC-1", 6, exchange_code="XNAS"),  # gap = 5 = lookback_days
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(), _options(lookback_days=5))
    assert len(out) == 2
    first, second = out.iloc[0], out.iloc[1]
    # extended = 1 + 5 - 1 = rank 5; next_rank - 1 = 6 - 1 = rank 5: they coincide, so the
    # bound is the full extension and the intervals still abut with no dropped day.
    assert first["valid_to"] == _rank_date(5)
    assert second["valid_from"] == _rank_date(6)
    assert first["valid_to"] == second["valid_from"] - dt.timedelta(days=1)


def test_unchanged_state_collapse_at_gap_equal_to_lookback_still_works():
    """Regression guard: the SAME boundary with an UNCHANGED state must still collapse
    into a single continuous interval (this path never calls close() mid-security, so
    the fix must not disturb it)."""

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", 1),
            _decision("SEC-1", 6),  # gap = 5 = lookback_days -> NOT gapped (> is strict)
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(), _options(lookback_days=5))
    assert len(out) == 1
    assert out.iloc[0]["valid_from"] == _rank_date(1)
    assert int(out.iloc[0]["decision_count"]) == 2


def test_gap_greater_than_lookback_leaves_a_real_hole():
    """A genuine gap (exceeding lookback_days) must still produce a real hole, not be
    bridged -- the bound only matters when the successor is within reach."""

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", 1, exchange_code="XNYS"),
            _decision("SEC-1", 7, exchange_code="XNAS"),  # gap = 6 > lookback_days (3)
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(), _options(lookback_days=3))
    assert len(out) == 2
    first, second = out.iloc[0], out.iloc[1]
    # extended = 1 + 3 - 1 = rank 3; next_rank - 1 = 7 - 1 = rank 6: extended is smaller,
    # so the bound is the plain extension and ranks 4-6 are a real, uncovered hole.
    assert first["valid_to"] == _rank_date(3)
    assert second["valid_from"] == _rank_date(7)
    assert first["valid_to"] < second["valid_from"] - dt.timedelta(days=1)


def test_has_cik_transition_bridges_with_no_dropped_days():
    """A has_cik (reason) transition within the lookback window must bridge too -- the
    bug wasn't specific to exchange_code."""

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", 1, has_cik=True, cik="0000320193"),
            _decision("SEC-1", 4, has_cik=False, cik=None),
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(), _options(lookback_days=5))
    assert len(out) == 2
    first, second = out.iloc[0], out.iloc[1]
    assert first["reason"] == "member"
    assert second["reason"] == "member_no_cik"
    assert first["valid_to"] == second["valid_from"] - dt.timedelta(days=1)


def test_security_type_only_transition_bridges_with_no_dropped_days():
    """A security_type-only transition (other three state columns unchanged) within the
    lookback window must bridge, same as any other _INTERVAL_STATE_COLUMNS change."""

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", 1, security_type="common"),
            _decision("SEC-1", 4, security_type="REIT"),
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(), _options(lookback_days=5))
    assert len(out) == 2
    first, second = out.iloc[0], out.iloc[1]
    assert first["security_type"] == "common"
    assert second["security_type"] == "REIT"
    assert first["valid_to"] == second["valid_from"] - dt.timedelta(days=1)


def test_final_close_with_no_successor_is_still_open_ended():
    """Regression guard: a state change immediately followed by the security's LAST
    decision (no third decision at all) must fall back to the open-ended/final-close
    behavior for that final interval, not be bounded by a nonexistent successor."""

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", 1, exchange_code="XNYS"),
            _decision("SEC-1", 4, exchange_code="XNAS"),
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(), _options(lookback_days=5))
    assert len(out) == 2
    second = out.iloc[1]
    # extended = 4 + 5 - 1 = rank 8, within the 12-session grid -> a concrete close date,
    # not open (archive end is rank 12).
    assert second["valid_to"] == _rank_date(8)
