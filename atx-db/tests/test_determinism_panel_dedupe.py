"""Audit §3.2 / §9: panel dedupe and breadth availability must not read the clock.

Note: the brief's addendum assumed a ``read_panel(frame, as_of_date=...)`` helper.
No such function exists in ``atx_db.factor_panel`` -- the pandas-path dedupe under
test lives in ``assemble_factor_panel_long(fundamental_factors=..., as_of_date=...)``,
which applies the same sort_values/drop_duplicates block. These tests call that
function directly (with no ``universe_membership``, which is a no-op filter) instead.
"""

from __future__ import annotations

import datetime as dt

import pandas as pd

from atx_db.factor_panel import FACTOR_PANEL_COLUMNS, assemble_factor_panel_long
from atx_db.signal_eval import compute_breadth


def _surface(load_stamps: tuple[str, str]) -> pd.DataFrame:
    """Two rows that are identical except for run_id and the wall-clock load stamp."""
    return pd.DataFrame(
        [
            {
                "security_id": "S1",
                "as_of_date": dt.date(2024, 1, 2),
                "factor_id": "F1",
                "value": 1.0,
                "available_at": pd.Timestamp("2024-01-02 22:00:00"),
                "source_loaded_at": pd.Timestamp(load_stamps[0]),
                "run_id": "run-a",
                "input_lineage_json": "{}",
            },
            {
                "security_id": "S1",
                "as_of_date": dt.date(2024, 1, 2),
                "factor_id": "F1",
                "value": 2.0,
                "available_at": pd.Timestamp("2024-01-02 22:00:00"),
                "source_loaded_at": pd.Timestamp(load_stamps[1]),
                "run_id": "run-b",
                "input_lineage_json": "{}",
            },
        ]
    )


def _read_panel(surface: pd.DataFrame, *, as_of_date: dt.date) -> pd.DataFrame:
    return assemble_factor_panel_long(fundamental_factors=surface, as_of_date=as_of_date)


def test_panel_contract_still_carries_the_load_stamp_column():
    assert "source_loaded_at" in FACTOR_PANEL_COLUMNS


def test_panel_dedupe_is_independent_of_the_load_stamp_order():
    early = _read_panel(
        _surface(("2024-01-03 01:00:00", "2024-01-03 02:00:00")),
        as_of_date=dt.date(2024, 1, 2),
    )
    late = _read_panel(
        _surface(("2024-01-03 09:00:00", "2024-01-03 03:00:00")),
        as_of_date=dt.date(2024, 1, 2),
    )
    assert len(early) == len(late) == 1
    assert early["run_id"].tolist() == late["run_id"].tolist() == ["run-b"]
    assert early["value"].tolist() == late["value"].tolist() == [2.0]


def test_panel_dedupe_prefers_the_later_available_at_over_any_run_id():
    surface = _surface(("2024-01-03 09:00:00", "2024-01-03 01:00:00"))
    surface.loc[0, "available_at"] = pd.Timestamp("2024-01-02 23:00:00")
    panel = _read_panel(surface, as_of_date=dt.date(2024, 1, 2))
    assert panel["run_id"].tolist() == ["run-a"]


def test_panel_dedupe_breaks_a_null_run_id_tie_toward_the_identified_row():
    surface = _surface(("2024-01-03 09:00:00", "2024-01-03 01:00:00"))
    surface.loc[0, "run_id"] = None
    panel = _read_panel(surface, as_of_date=dt.date(2024, 1, 2))
    assert panel["run_id"].tolist() == ["run-b"]


def _breadth_panel(available: list[object]) -> pd.DataFrame:
    return pd.DataFrame(
        {
            "security_id": ["S1", "S2"],
            "as_of_date": [dt.date(2024, 1, 2)] * 2,
            "factor_id": ["F1"] * 2,
            "value": [1.0, 2.0],
            "available_at": available,
        }
    )


def test_breadth_available_at_is_the_max_of_input_availability():
    breadth = compute_breadth(
        _breadth_panel([pd.Timestamp("2024-01-02 22:00:00"), pd.Timestamp("2024-01-03 06:00:00")])
    )
    assert breadth["available_at"].tolist() == [pd.Timestamp("2024-01-03 06:00:00")]


def test_breadth_falls_back_to_as_of_date_plus_22h_when_availability_is_absent():
    panel = _breadth_panel([pd.NaT, pd.NaT]).drop(columns=["available_at"])
    breadth = compute_breadth(panel)
    assert breadth["available_at"].tolist() == [pd.Timestamp("2024-01-02 22:00:00")]


def test_breadth_falls_back_per_group_when_every_input_availability_is_null():
    breadth = compute_breadth(_breadth_panel([pd.NaT, pd.NaT]))
    assert breadth["available_at"].tolist() == [pd.Timestamp("2024-01-02 22:00:00")]


def test_breadth_is_byte_reproducible_across_repeated_calls():
    panel = _breadth_panel([pd.NaT, pd.NaT]).drop(columns=["available_at"])
    first = compute_breadth(panel)
    second = compute_breadth(panel)
    pd.testing.assert_frame_equal(first, second)


def test_breadth_is_independent_of_input_row_order():
    panel = _breadth_panel(
        [pd.Timestamp("2024-01-02 22:00:00"), pd.Timestamp("2024-01-03 06:00:00")]
    )
    forward = compute_breadth(panel)
    reversed_ = compute_breadth(panel.iloc[::-1].reset_index(drop=True))
    pd.testing.assert_frame_equal(forward, reversed_)
