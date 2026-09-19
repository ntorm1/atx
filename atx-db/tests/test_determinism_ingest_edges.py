"""Audit §9 group B: ingest loaders take an explicit or source-derived as_of_date."""

from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from atx_db.finra import FinraShortInterestDataset, FinraShortInterestOptions
from atx_db.identifiers_figi import FigiLoadOptions
from atx_db.identifiers_lei import LeiLoadOptions
from atx_db.security_master import SecurityMasterOptions, upsert_security_master_from_frame
from atx_db.symbol_directory import (
    NasdaqSymbolDirectoryOptions,
    resolve_directory_as_of_date,
)

_DIRECTORY_TEXT = (
    "Symbol|Security Name|Market Category|Test Issue|Financial Status|Round Lot Size\n"
    "AAPL|Apple Inc. - Common Stock|Q|N|N|100\n"
    "File Creation Time: 0630202422:01|||||\n"
)


def test_security_master_options_expose_an_as_of_date():
    assert SecurityMasterOptions().as_of_date is None
    assert SecurityMasterOptions(as_of_date=dt.date(2024, 5, 1)).as_of_date == dt.date(2024, 5, 1)


def test_upsert_security_master_requires_an_explicit_as_of_date(tmp_store):
    frame = pd.DataFrame(
        [{"cik": "0000320193", "ticker": "AAPL", "title": "Apple Inc.", "security_id": "SEC-CIK-0000320193"}]
    )
    with pytest.raises(TypeError):
        upsert_security_master_from_frame(tmp_store, frame, source="unit-test")  # type: ignore[call-arg]


def test_upsert_security_master_stamps_the_supplied_as_of_date(tmp_store):
    frame = pd.DataFrame(
        [{"cik": "0000320193", "ticker": "AAPL", "title": "Apple Inc.", "security_id": "SEC-CIK-0000320193"}]
    )
    upsert_security_master_from_frame(
        tmp_store, frame, source="unit-test", as_of_date=dt.date(2021, 7, 4), run_id="r1"
    )
    row = tmp_store.con.execute(
        "SELECT min(as_of_date) FROM security_identifier_history WHERE source = 'unit-test'"
    ).fetchone()
    assert row is not None and row[0] == dt.date(2021, 7, 4)


def test_directory_as_of_date_comes_from_the_nasdaq_file_creation_time():
    assert resolve_directory_as_of_date(
        NasdaqSymbolDirectoryOptions(), _DIRECTORY_TEXT
    ) == dt.date(2024, 6, 30)


def test_explicit_directory_as_of_date_wins():
    assert resolve_directory_as_of_date(
        NasdaqSymbolDirectoryOptions(as_of_date=dt.date(2020, 1, 1)), _DIRECTORY_TEXT
    ) == dt.date(2020, 1, 1)


def test_directory_without_creation_time_or_explicit_date_raises():
    with pytest.raises(ValueError, match="as_of_date is required"):
        resolve_directory_as_of_date(NasdaqSymbolDirectoryOptions(), "Symbol|Security Name\nAAPL|Apple\n")


def test_figi_load_without_an_as_of_date_raises(tmp_store, tmp_path):
    # The brief names a `FigiDataset`; the real class is `FigiAliasDataset`
    # (see identifiers_figi.py) -- substituted here, same requirement intent.
    from atx_db.identifiers_figi import FigiAliasDataset

    figi_file = tmp_path / "figi.csv"
    figi_file.write_text("cusip,figi\n037833100,BBG000B9XRY4\n", encoding="utf-8")
    with pytest.raises(ValueError, match="as_of_date is required"):
        FigiAliasDataset().load(tmp_store, FigiLoadOptions(figi_file=figi_file))


def test_lei_load_without_an_as_of_date_raises(tmp_store, tmp_path):
    # The brief names a `LeiDataset`; the real class is `LeiAliasDataset`
    # (see identifiers_lei.py) -- substituted here, same requirement intent.
    from atx_db.identifiers_lei import LeiAliasDataset

    lei_file = tmp_path / "lei.csv"
    lei_file.write_text("lei,cik\n549300FJ4DPCQZQCXQ36,0000320193\n", encoding="utf-8")
    with pytest.raises(ValueError, match="as_of_date is required"):
        LeiAliasDataset().load(tmp_store, LeiLoadOptions(lei_file=lei_file))


def test_finra_load_without_a_date_window_raises(tmp_store):
    with pytest.raises(ValueError, match="start_date and end_date are required"):
        FinraShortInterestDataset().load(tmp_store, FinraShortInterestOptions())


def test_no_wall_clock_reads_remain_in_the_group_b_modules():
    import pathlib

    root = pathlib.Path(__file__).resolve().parents[1] / "src" / "atx_db"
    offenders: list[str] = []
    for name in (
        "security_master.py",
        "symbol_directory.py",
        "identifiers_figi.py",
        "identifiers_lei.py",
        "finra.py",
    ):
        text = (root / name).read_text(encoding="utf-8")
        for needle in ("date.today()", "datetime.utcnow()", "Timestamp.now("):
            if needle in text:
                offenders.append(f"{name}: {needle}")
    assert offenders == [], offenders


# ---------------------------------------------------------------------------
# jobs.py options-builders are the job-entry edge for the security-master and
# FINRA date-window datasets: since these loaders now fail shut without an
# explicit or source-derived as_of_date / date window, these builders must
# always produce a usable value, defaulting to atx_db.clock.utc_today() (the
# one sanctioned wall-clock read at this edge) when the job params don't
# supply one. Mirrors test_determinism_as_of.py's job-edge coverage for
# group A (Task 1).
# ---------------------------------------------------------------------------


def test_security_master_job_options_default_as_of_date_to_utc_today(monkeypatch):
    import atx_db.jobs as jobs_module
    from atx_db.security_master import SecurityMasterDataset

    sentinel = dt.date(2024, 5, 1)
    monkeypatch.setattr(jobs_module, "utc_today", lambda: sentinel)
    _dataset_cls, build_options = jobs_module.DATASET_REGISTRY[SecurityMasterDataset.dataset_id]
    options = build_options({})
    assert options.as_of_date == sentinel


def test_security_master_job_options_honor_an_explicit_as_of_date_param(monkeypatch):
    import atx_db.jobs as jobs_module
    from atx_db.security_master import SecurityMasterDataset

    monkeypatch.setattr(
        jobs_module,
        "utc_today",
        lambda: pytest.fail("clock read despite an explicit as_of_date param"),
    )
    _dataset_cls, build_options = jobs_module.DATASET_REGISTRY[SecurityMasterDataset.dataset_id]
    options = build_options({"as_of_date": "2020-01-02"})
    assert options.as_of_date == dt.date(2020, 1, 2)


def test_finra_job_options_default_date_window_to_utc_today_when_no_symbol(monkeypatch):
    import atx_db.jobs as jobs_module

    sentinel = dt.date(2024, 5, 1)
    monkeypatch.setattr(jobs_module, "utc_today", lambda: sentinel)
    _dataset_cls, build_options = jobs_module.DATASET_REGISTRY[FinraShortInterestDataset.dataset_id]
    options = build_options({})
    assert options.start_date == dt.date(2019, 5, 1)
    assert options.end_date == sentinel


def test_finra_job_options_honor_explicit_date_window_params(monkeypatch):
    import atx_db.jobs as jobs_module

    monkeypatch.setattr(
        jobs_module,
        "utc_today",
        lambda: pytest.fail("clock read despite explicit start_date/end_date params"),
    )
    _dataset_cls, build_options = jobs_module.DATASET_REGISTRY[FinraShortInterestDataset.dataset_id]
    options = build_options({"start_date": "2020-01-01", "end_date": "2020-01-31"})
    assert options.start_date == dt.date(2020, 1, 1)
    assert options.end_date == dt.date(2020, 1, 31)


def test_finra_job_options_leave_the_date_window_unset_in_symbol_mode(monkeypatch):
    import atx_db.jobs as jobs_module

    monkeypatch.setattr(
        jobs_module,
        "utc_today",
        lambda: pytest.fail("clock read despite --symbol mode not needing a date window"),
    )
    _dataset_cls, build_options = jobs_module.DATASET_REGISTRY[FinraShortInterestDataset.dataset_id]
    options = build_options({"symbol": "AAPL"})
    assert options.start_date is None
    assert options.end_date is None
