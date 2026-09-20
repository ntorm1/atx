from __future__ import annotations

import datetime as dt
from collections.abc import Iterator

import duckdb
import pandas as pd
import pytest

from atx_db import calendarization
from atx_db.calendarization import (
    CALENDAR_MAP_COLUMNS,
    CalendarizationOptions,
    compute_calendar_map_rows,
    refresh_fundamental_calendar_map,
)
from atx_db.connection import DuckDBStore
from atx_db.warehouse import insert_frame


@pytest.fixture
def calendar_store() -> Iterator[DuckDBStore]:
    """Only calendar-map inputs/output, with one thread and a 128 MB SQL budget."""
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(config={"threads": 1, "memory_limit": "128MB"})
    store.con.execute("SET preserve_insertion_order = false")
    store.con.execute(
        """
        CREATE TABLE fundamental_periods (
            fundamental_period_id VARCHAR PRIMARY KEY, period_group_id VARCHAR,
            source VARCHAR, security_id VARCHAR, symbol VARCHAR, cik VARCHAR,
            accession_number VARCHAR, period_start DATE, period_end DATE,
            normalized_period_type VARCHAR, reported_fiscal_years_json VARCHAR,
            reported_fiscal_periods_json VARCHAR, as_of_date DATE,
            available_at TIMESTAMP, is_latest_revision BOOLEAN, source_loaded_at TIMESTAMP
        );
        CREATE TABLE fundamental_calendar_map (
            calendar_map_id VARCHAR PRIMARY KEY, source VARCHAR NOT NULL,
            upstream_source VARCHAR NOT NULL, fundamental_period_id VARCHAR NOT NULL,
            period_group_id VARCHAR NOT NULL, security_id VARCHAR NOT NULL, symbol VARCHAR,
            cik VARCHAR NOT NULL, accession_number VARCHAR NOT NULL,
            period_start DATE, period_end DATE NOT NULL, normalized_period_type VARCHAR NOT NULL,
            fyr INTEGER NOT NULL, period_length_days INTEGER, week_count INTEGER,
            is_53_week BOOLEAN NOT NULL, reported_fiscal_year INTEGER,
            reported_fiscal_period VARCHAR, fiscal_scheme_year INTEGER NOT NULL,
            fiscal_scheme_quarter INTEGER NOT NULL, fiscal_scheme_period VARCHAR,
            containing_calendar_year INTEGER NOT NULL, containing_calendar_quarter INTEGER NOT NULL,
            containing_calendar_period VARCHAR, greatest_overlap_calendar_year INTEGER NOT NULL,
            greatest_overlap_calendar_quarter INTEGER NOT NULL,
            greatest_overlap_calendar_period VARCHAR, as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL, is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR, source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            CHECK (symbol IS NULL OR symbol <> '__reject_publication__')
        )
        """
    )
    try:
        yield store
    finally:
        store.con.close()


def _insert_period(
    store: DuckDBStore,
    period_id: str,
    *,
    source: str = "SEC",
    security_id: str = "issuer",
    start: str | None = "2024-01-01",
    end: str | None = "2024-03-31",
    period_type: str = "quarter",
    years: str | None = "[]",
    periods: str | None = "[]",
    latest: bool = True,
) -> None:
    store.con.execute(
        "INSERT INTO fundamental_periods VALUES "
        "(?, ?, ?, ?, 'SYM', '123', ?, ?, ?, ?, ?, ?, DATE '2025-08-01', "
        "TIMESTAMP '2025-08-01 22:00:00.123456', ?, TIMESTAMP '2025-08-02 01:23:45.654321')",
        [period_id, f"group-{period_id}", source, security_id, f"accession-{period_id}",
         start, end, period_type, years, periods, latest],
    )


def _stored_rows(store: DuckDBStore) -> list[tuple[object, ...]]:
    return store.con.execute(
        f"SELECT {', '.join(CALENDAR_MAP_COLUMNS)} "
        "FROM fundamental_calendar_map ORDER BY calendar_map_id"
    ).fetchall()


def _assert_no_staging(store: DuckDBStore) -> None:
    assert store.con.execute(
        "SELECT table_name FROM information_schema.tables "
        "WHERE table_name IN ('_calendar_map_input', '_calendar_map_output', '_calendar_map_rows')"
    ).fetchall() == []


def test_batches_match_original_oracle_with_full_issuer_inference(calendar_store) -> None:
    _insert_period(calendar_store, "annual-old", start="2022-01-01", end="2022-12-31",
                   period_type="annual", years='["bad", "2022"]', periods='["FY"]')
    _insert_period(calendar_store, "annual-new", start="2024-05-26", end="2025-05-31",
                   period_type="annual", years="[]", periods="FY")
    # Tied latest annual periods must retain their common FYR without depending on row order.
    _insert_period(calendar_store, "annual-tie", start=None, end="2025-05-31",
                   period_type="annual", years="2025", periods='["ANNUAL"]', latest=False)
    _insert_period(calendar_store, "quarter", start="2024-02-01", end="2024-05-01",
                   years='["2024", "2025"]', periods='["Q1_DERIVED"]')
    _insert_period(calendar_store, "stub", start="2024-12-31", end="2025-01-14",
                   period_type="stub", years=None, periods=None)
    _insert_period(calendar_store, "instant", start=None, end="2024-10-31",
                   period_type="instant", periods='[null, "Q3"]')
    _insert_period(calendar_store, "semiannual", start="2024-01-01", end="2024-07-07",
                   period_type="semiannual_ytd", periods='"Q2"')
    _insert_period(calendar_store, "multi-quarter", start="2024-01-01", end="2024-10-06",
                   period_type="multi_quarter_ytd", periods="Q3")
    _insert_period(calendar_store, "no-end", end=None)
    _insert_period(calendar_store, "other-source-year", source="Other", start="2023-07-01",
                   end="2024-06-30", period_type="annual")
    _insert_period(calendar_store, "other-source-quarter", source="Other")
    _insert_period(calendar_store, "no-annual", security_id="issuer-without-annual")
    # This is the original whole-input query, used only as a small fixture oracle.
    periods = calendar_store.con.execute(
        """
        WITH issuer_fyr AS (
            SELECT source, security_id,
                   arg_max(CAST(EXTRACT(MONTH FROM period_end) AS INTEGER), period_end) AS fyr
            FROM fundamental_periods
            WHERE normalized_period_type = 'annual' AND period_end IS NOT NULL
            GROUP BY 1, 2
        )
        SELECT fp.*, coalesce(issuer_fyr.fyr, CAST(EXTRACT(MONTH FROM fp.period_end) AS INTEGER)) fyr
        FROM fundamental_periods fp LEFT JOIN issuer_fyr
          ON issuer_fyr.source = fp.source AND issuer_fyr.security_id = fp.security_id
        """
    ).df()
    options = CalendarizationOptions(source="labels", run_id="fixed-run")
    expected = compute_calendar_map_rows(periods, source=options.source, run_id=options.run_id)
    insert_frame(calendar_store, expected, "fundamental_calendar_map", "_expected_calendar_rows")
    original_rows = _stored_rows(calendar_store)

    assert refresh_fundamental_calendar_map(calendar_store, options) == 11
    assert _stored_rows(calendar_store) == original_rows
    assert calendar_store.con.execute(
        "SELECT fundamental_period_id, fyr FROM fundamental_calendar_map "
        "WHERE fundamental_period_id IN ('quarter', 'other-source-quarter', 'no-annual') "
        "ORDER BY fundamental_period_id"
    ).fetchall() == [("no-annual", 3), ("other-source-quarter", 6), ("quarter", 5)]
    assert calendar_store.con.execute(
        "SELECT available_at, source_loaded_at, run_id FROM fundamental_calendar_map LIMIT 1"
    ).fetchone() == (dt.datetime(2025, 8, 1, 22, 0, 0, 123456),
                     dt.datetime(2025, 8, 2, 1, 23, 45, 654321), "fixed-run")
    assert refresh_fundamental_calendar_map(calendar_store, options) == 11
    assert _stored_rows(calendar_store) == original_rows
    _assert_no_staging(calendar_store)


def _insert_large_issuer(store: DuckDBStore) -> int:
    count = 2 * calendarization._CALENDAR_MAP_BATCH_SIZE + 3
    store.con.execute(
        """
        INSERT INTO fundamental_periods
        SELECT 'quarter-' || lpad(i::VARCHAR, 6, '0'), 'group-' || i, 'SEC', 'issuer', 'SYM',
               '123', 'accession-' || i, DATE '2024-01-01', DATE '2024-03-31', 'quarter',
               '[]', '[]', DATE '2025-08-01', TIMESTAMP '2025-08-01 22:00:00',
               true, TIMESTAMP '2025-08-02 01:23:45'
        FROM range(?) t(i)
        """,
        [count],
    )
    _insert_period(store, "z-latest-annual", start="2024-06-01", end="2025-05-31",
                   period_type="annual")
    return count + 1


def test_large_issuer_crosses_batches_without_losing_latest_annual(calendar_store, monkeypatch) -> None:
    count = _insert_large_issuer(calendar_store)
    batch_sizes: list[int] = []
    original_df = duckdb.DuckDBPyConnection.df

    def bounded_dataframe(connection: duckdb.DuckDBPyConnection) -> pd.DataFrame:
        frame = original_df(connection)
        assert len(frame) <= calendarization._CALENDAR_MAP_BATCH_SIZE
        return frame

    def bounded_oracle(periods: pd.DataFrame, **kwargs: object) -> pd.DataFrame:
        batch_sizes.append(len(periods))
        assert 0 < len(periods) <= calendarization._CALENDAR_MAP_BATCH_SIZE
        assert periods["fyr"].eq(5).all()
        return compute_calendar_map_rows(periods, **kwargs)

    monkeypatch.setattr(calendarization, "compute_calendar_map_rows", bounded_oracle)
    monkeypatch.setattr(duckdb.DuckDBPyConnection, "df", bounded_dataframe)
    monkeypatch.setattr(duckdb.DuckDBPyConnection, "fetchdf", bounded_dataframe)
    assert refresh_fundamental_calendar_map(calendar_store) == count
    assert len(batch_sizes) == 3
    assert sum(batch_sizes) == count
    assert calendar_store.con.execute(
        "SELECT count(*), count(DISTINCT calendar_map_id), min(fyr), max(fyr) "
        "FROM fundamental_calendar_map"
    ).fetchone() == (count, count, 5, 5)
    _assert_no_staging(calendar_store)


def test_source_replacement_including_empty_preserves_other_sources(calendar_store) -> None:
    _insert_period(calendar_store, "first")
    assert refresh_fundamental_calendar_map(calendar_store, CalendarizationOptions(source="one")) == 1
    assert refresh_fundamental_calendar_map(calendar_store, CalendarizationOptions(source="two")) == 1
    calendar_store.con.execute("DELETE FROM fundamental_periods")
    # A missing end date yields an empty output even though an input batch exists.
    _insert_period(calendar_store, "no-end", end=None)
    assert refresh_fundamental_calendar_map(calendar_store, CalendarizationOptions(source="one")) == 0
    assert calendar_store.con.execute(
        "SELECT source, fundamental_period_id FROM fundamental_calendar_map"
    ).fetchall() == [("two", "first")]
    calendar_store.con.execute("DELETE FROM fundamental_periods")
    assert refresh_fundamental_calendar_map(calendar_store, CalendarizationOptions(source="two")) == 0
    assert _stored_rows(calendar_store) == []
    _assert_no_staging(calendar_store)


def test_publication_failure_rolls_back_replacement_and_temp_tables(calendar_store) -> None:
    _insert_period(calendar_store, "original")
    assert refresh_fundamental_calendar_map(calendar_store) == 1
    before = _stored_rows(calendar_store)
    calendar_store.con.execute("UPDATE fundamental_periods SET symbol = '__reject_publication__'")

    with pytest.raises(duckdb.ConstraintException, match="CHECK constraint failed"):
        refresh_fundamental_calendar_map(calendar_store)

    assert _stored_rows(calendar_store) == before
    _assert_no_staging(calendar_store)


def test_later_batch_failure_rolls_back_staged_chunks(calendar_store, monkeypatch) -> None:
    _insert_period(calendar_store, "original")
    assert refresh_fundamental_calendar_map(calendar_store) == 1
    before = _stored_rows(calendar_store)
    _insert_large_issuer(calendar_store)
    calls = 0

    def failed_oracle(periods: pd.DataFrame, **kwargs: object) -> pd.DataFrame:
        nonlocal calls
        calls += 1
        if calls == 2:
            raise ValueError("forced later-batch failure")
        return compute_calendar_map_rows(periods, **kwargs)

    monkeypatch.setattr(calendarization, "compute_calendar_map_rows", failed_oracle)
    with pytest.raises(ValueError, match="forced later-batch failure"):
        refresh_fundamental_calendar_map(calendar_store)

    assert calls == 2
    assert _stored_rows(calendar_store) == before
    _assert_no_staging(calendar_store)
