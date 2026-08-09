from __future__ import annotations

import datetime as dt

import pandas as pd


def test_value_unit_multiplier_pre_2023_is_thousands():
    from atx_db.thirteenf import value_unit_multiplier

    # SEC 13F VALUE was reported in thousands of dollars for periodOfReport
    # on/before 2022-12-31 (amended rules took effect for 2023 filings).
    assert value_unit_multiplier(dt.date(2022, 12, 31)) == 1000
    assert value_unit_multiplier(dt.date(2020, 6, 30)) == 1000


def test_value_unit_multiplier_2023_onward_is_whole_dollars():
    from atx_db.thirteenf import value_unit_multiplier

    assert value_unit_multiplier(dt.date(2023, 1, 1)) == 1
    assert value_unit_multiplier(dt.date(2026, 3, 31)) == 1


def test_value_unit_multiplier_missing_period_defaults_to_one():
    from atx_db.thirteenf import value_unit_multiplier

    assert value_unit_multiplier(None) == 1
    assert value_unit_multiplier(pd.NaT) == 1


def test_apply_value_unit_cutover_scales_only_pre_2023():
    from atx_db.thirteenf import apply_value_unit_cutover

    frame = pd.DataFrame(
        {
            "accession_number": ["acc-old", "acc-new", "acc-unknown"],
            "value_usd": [10.0, 10.0, 10.0],
        }
    )
    accession_periods = {
        "acc-old": dt.date(2022, 9, 30),   # thousands -> x1000
        "acc-new": dt.date(2023, 3, 31),   # whole dollars -> x1
        # acc-unknown intentionally absent -> no scaling (safe default)
    }

    out = apply_value_unit_cutover(frame, accession_periods)

    assert out.loc[0, "value_usd"] == 10000.0
    assert out.loc[1, "value_usd"] == 10.0
    assert out.loc[2, "value_usd"] == 10.0


def test_apply_value_unit_cutover_empty_frame_is_noop():
    from atx_db.thirteenf import apply_value_unit_cutover

    empty = pd.DataFrame(columns=["accession_number", "value_usd"])
    out = apply_value_unit_cutover(empty, {})
    assert out.empty
