"""Audit §9 determinism fixes: no wall-clock reads on reproducible row builders."""

from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from atx_db.fundamentals import _unresolved_cik_candidates
from atx_db.identifier_decisions import IdentifierResolutionDecisionOptions
from atx_db.identifier_resolution import IdentifierResolutionOptions
from atx_db.valuation_multiples import ValuationMultiplesOptions, _overlap_slice_row


def test_overlap_slice_row_falls_back_to_now_utc_naive_not_utcnow(monkeypatch):
    sentinel = dt.datetime(2024, 6, 30, 12, 0, 0)
    monkeypatch.setattr("atx_db.valuation_multiples.now_utc_naive", lambda: sentinel)
    row = _overlap_slice_row(ValuationMultiplesOptions(), {})
    assert row["available_at"] == sentinel
    assert row["as_of_date"] == dt.date(2024, 6, 30)


def test_overlap_slice_row_prefers_source_timestamps_over_the_clock(monkeypatch):
    monkeypatch.setattr(
        "atx_db.valuation_multiples.now_utc_naive",
        lambda: pytest.fail("clock read despite available source timestamps"),
    )
    row = _overlap_slice_row(
        ValuationMultiplesOptions(),
        {"as_of_ts": "2023-03-31T22:00:00", "max_valuation_trade_date": "2023-03-31"},
    )
    assert row["available_at"] == dt.datetime(2023, 3, 31, 22, 0, 0)
    assert row["as_of_date"] == dt.date(2023, 3, 31)


def test_unresolved_cik_candidates_requires_an_explicit_as_of_date():
    unresolved = pd.DataFrame(
        [{"cik": "0000320193", "security_id": "SEC-CIK-0000320193", "available_at": pd.Timestamp("2024-02-01 22:00:00")}]
    )
    with pytest.raises(TypeError):
        _unresolved_cik_candidates(unresolved, run_id="r1")  # type: ignore[call-arg]


def test_unresolved_cik_candidates_stamps_the_supplied_as_of_date():
    unresolved = pd.DataFrame(
        [{"cik": "0000320193", "security_id": "SEC-CIK-0000320193", "available_at": pd.Timestamp("2024-02-01 22:00:00")}]
    )
    frame = _unresolved_cik_candidates(unresolved, run_id="r1", as_of_date=dt.date(2024, 2, 1))
    assert list(frame["as_of_date"]) == [dt.date(2024, 2, 1)]


def test_identifier_resolution_options_expose_an_as_of_date():
    assert IdentifierResolutionOptions().as_of_date is None
    assert IdentifierResolutionOptions(as_of_date=dt.date(2024, 5, 1)).as_of_date == dt.date(2024, 5, 1)


def test_identifier_decision_options_expose_an_as_of_date():
    assert IdentifierResolutionDecisionOptions().as_of_date is None
    assert (
        IdentifierResolutionDecisionOptions(as_of_date=dt.date(2024, 5, 1)).as_of_date
        == dt.date(2024, 5, 1)
    )


def test_no_wall_clock_reads_remain_in_the_group_a_modules():
    import pathlib

    root = pathlib.Path(__file__).resolve().parents[1] / "src" / "atx_db"
    offenders: list[str] = []
    for name in (
        "valuation_multiples.py",
        "delisting.py",
        "identifier_resolution.py",
        "identifier_decisions.py",
        "fundamentals.py",
        "signal_eval.py",
    ):
        text = (root / name).read_text(encoding="utf-8")
        for needle in ("date.today()", "datetime.utcnow()", "datetime.now(", "Timestamp.now("):
            if needle in text:
                offenders.append(f"{name}: {needle}")
    assert offenders == [], offenders


def test_factor_panel_never_orders_on_the_load_stamp():
    import pathlib
    import re

    text = (
        pathlib.Path(__file__).resolve().parents[1] / "src" / "atx_db" / "factor_panel.py"
    ).read_text(encoding="utf-8")
    assert '"source_loaded_at"' in text, "the pinned 8-column contract must keep the column"
    assert "source_loaded_at DESC" not in text
    assert not re.search(r'sort_values\(\s*\[[^\]]*"source_loaded_at"', text, flags=re.S)
