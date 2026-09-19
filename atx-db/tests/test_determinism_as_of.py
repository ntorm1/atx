"""Audit §9 determinism fixes: no wall-clock reads on reproducible row builders."""

from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from atx_db.fundamentals import _unresolved_cik_candidates
from atx_db.identifier_decisions import (
    IdentifierResolutionDecisionDataset,
    IdentifierResolutionDecisionOptions,
)
from atx_db.identifier_resolution import (
    IdentifierResolutionCandidateDataset,
    IdentifierResolutionOptions,
)
from atx_db.thirteenf import ThirteenFDataSet
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


# ---------------------------------------------------------------------------
# _build_candidates (identifier_resolution.py) exercised through a real store:
# the source-derived filing/report date wins when present; the operator's
# options.as_of_date is only a fallback for rows with no source date; and
# with neither, the row builder must fail shut (never fall back to the SQL
# wall clock -- see the current_date() removal in _source_cusips below).
# ---------------------------------------------------------------------------


def _seed_thirteenf_candidate_source(store, *, with_submission: bool) -> None:
    """The smallest thirteenf_holdings/sec_company_tickers fixture that
    _build_candidates can turn into exactly one candidate row."""
    ThirteenFDataSet().ensure_schema(store)
    if with_submission:
        store.con.execute(
            """
            INSERT INTO thirteenf_submissions (accession_number, filing_date, period_of_report, source_period)
            VALUES ('ACC-1', DATE '2020-05-15', DATE '2020-03-31', '2024Q1')
            """
        )
    store.con.execute(
        """
        INSERT INTO thirteenf_holdings (accession_number, security_id, name_of_issuer, cusip, source_period)
        VALUES ('ACC-1', NULL, 'Apple Inc', '037833100', '2024Q1')
        """
    )
    store.con.execute(
        """
        INSERT INTO sec_company_tickers (cik, ticker, title, security_id)
        VALUES ('0000320193', 'AAPL', 'Apple Inc', 'SEC-CIK-0000320193')
        """
    )


def test_build_candidates_uses_the_source_filing_date_over_options_as_of_date(tmp_store):
    """(a) A row with its own source-derived date keeps that date -- it is the
    more precise, genuinely PIT date for that row -- even when the operator
    also supplied options.as_of_date."""
    _seed_thirteenf_candidate_source(tmp_store, with_submission=True)
    dataset = IdentifierResolutionCandidateDataset()
    options = IdentifierResolutionOptions(min_confidence=0.0, as_of_date=dt.date(2099, 1, 1))
    frame = dataset._build_candidates(tmp_store, options)
    assert list(frame["as_of_date"]) == [dt.date(2020, 5, 15)]


def test_build_candidates_falls_back_to_options_as_of_date_without_a_source_date(tmp_store):
    """(b) No matching thirteenf_submissions row means no source-derived date;
    the row must then carry the operator-supplied options.as_of_date, not the
    wall clock."""
    _seed_thirteenf_candidate_source(tmp_store, with_submission=False)
    dataset = IdentifierResolutionCandidateDataset()
    options = IdentifierResolutionOptions(min_confidence=0.0, as_of_date=dt.date(2020, 1, 1))
    frame = dataset._build_candidates(tmp_store, options)
    assert list(frame["as_of_date"]) == [dt.date(2020, 1, 1)]


def test_build_candidates_raises_without_a_source_date_or_options_as_of_date(tmp_store):
    """(c) With neither a source-derived date nor options.as_of_date, the
    builder must fail shut rather than silently reading the clock."""
    _seed_thirteenf_candidate_source(tmp_store, with_submission=False)
    dataset = IdentifierResolutionCandidateDataset()
    options = IdentifierResolutionOptions(min_confidence=0.0, as_of_date=None)
    with pytest.raises(ValueError, match="as_of_date is required"):
        dataset._build_candidates(tmp_store, options)


# ---------------------------------------------------------------------------
# _build_decisions (identifier_decisions.py) exercised through a real store.
# identifier_resolution_candidates.as_of_date is DATE NOT NULL, so every real
# candidate row already carries a resolved date by the time _build_decisions
# reads it -- that row date always wins over options.as_of_date, and the
# "no source date" / "neither" branches of resolve_as_of_date are therefore
# structurally unreachable here (the schema itself is the fail-shut guard).
# Those two branches are exercised directly against resolve_as_of_date in
# test_clock.py; this test confirms _build_decisions actually calls through
# to the shared contract rather than reading the clock on its own.
# ---------------------------------------------------------------------------


def _seed_identifier_resolution_candidate(store, *, as_of_date: dt.date) -> None:
    store.con.execute(
        """
        INSERT INTO identifier_resolution_candidates (
            candidate_id, source_dataset_id, source_table, source_period,
            source_key_type, source_key_value, source_security_id,
            target_security_id, target_id_type, target_id_value,
            match_method, confidence, candidate_status, as_of_date, available_at
        )
        VALUES (
            'cand-1', 'sec_13f', 'thirteenf_holdings', '2024Q1',
            'CUSIP', '037833100', 'SEC-CIK-0000320193',
            'SEC-CIK-0000320193', 'CIK', '0000320193',
            'issuer_name_exact_normalized', 0.98, 'already_mapped', ?, ?
        )
        """,
        [as_of_date, dt.datetime.combine(as_of_date, dt.time(22, 0, 0))],
    )


def test_build_decisions_uses_the_candidate_row_as_of_date_not_options(tmp_store):
    _seed_identifier_resolution_candidate(tmp_store, as_of_date=dt.date(2020, 5, 15))
    dataset = IdentifierResolutionDecisionDataset()
    options = IdentifierResolutionDecisionOptions(as_of_date=dt.date(2099, 1, 1))
    decisions = dataset._build_decisions(tmp_store, options)
    assert list(decisions["as_of_date"]) == [dt.date(2020, 5, 15)]


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


# ---------------------------------------------------------------------------
# jobs.py options-builders are the job-entry edge for the identifier
# resolution/decision datasets: since _build_candidates now fails shut with
# no source date and no options.as_of_date, these builders must always
# produce a non-None as_of_date, defaulting to atx_db.clock.utc_today() (the
# one sanctioned wall-clock read at this edge) when the job params don't
# supply one.
# ---------------------------------------------------------------------------


def test_identifier_resolution_job_options_default_as_of_date_to_utc_today(monkeypatch):
    import atx_db.jobs as jobs_module

    sentinel = dt.date(2024, 5, 1)
    monkeypatch.setattr(jobs_module, "utc_today", lambda: sentinel)
    _dataset_cls, build_options = jobs_module.DATASET_REGISTRY[IdentifierResolutionCandidateDataset.dataset_id]
    options = build_options({})
    assert options.as_of_date == sentinel


def test_identifier_resolution_job_options_honor_an_explicit_as_of_date_param(monkeypatch):
    import atx_db.jobs as jobs_module

    monkeypatch.setattr(
        jobs_module,
        "utc_today",
        lambda: pytest.fail("clock read despite an explicit as_of_date param"),
    )
    _dataset_cls, build_options = jobs_module.DATASET_REGISTRY[IdentifierResolutionCandidateDataset.dataset_id]
    options = build_options({"as_of_date": "2020-01-02"})
    assert options.as_of_date == dt.date(2020, 1, 2)


def test_identifier_decision_job_options_default_as_of_date_to_utc_today(monkeypatch):
    import atx_db.jobs as jobs_module

    sentinel = dt.date(2024, 5, 1)
    monkeypatch.setattr(jobs_module, "utc_today", lambda: sentinel)
    _dataset_cls, build_options = jobs_module.DATASET_REGISTRY[IdentifierResolutionDecisionDataset.dataset_id]
    options = build_options({})
    assert options.as_of_date == sentinel
