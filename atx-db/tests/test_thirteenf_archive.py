from __future__ import annotations

import datetime as dt
import zipfile
from pathlib import Path

import duckdb
import pytest

from atx_db.connection import DuckDBStore
from atx_db.thirteenf import ThirteenFDataSet
from atx_db.thirteenf_archive import (
    ThirteenFArchiveBackfillOptions,
    archive_backfill_result,
    archive_from_url,
    archive_manifest_sha256,
    load_archive,
    select_archives,
)


@pytest.fixture
def archive_store():
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(":memory:")
    ThirteenFDataSet().ensure_schema(store)
    store.con.execute(
        """
        CREATE TABLE raw_source_files (
            source_id VARCHAR, dataset_id VARCHAR, source_url VARCHAR,
            cache_path VARCHAR, sha256 VARCHAR, byte_count BIGINT,
            fetched_at TIMESTAMP, status VARCHAR, metadata_json VARCHAR
        )
        """
    )
    try:
        yield store
    finally:
        store.connection.close()
        store.connection = None


def _tsv(headers: tuple[str, ...], rows: tuple[tuple[object, ...], ...]) -> str:
    lines = ["\t".join(headers)]
    lines.extend("\t".join(str(value) for value in row) for row in rows)
    return "\n".join(lines) + "\n"


def _write_tiny_archive(path: Path, *, prefix: str = "") -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(
            f"{prefix}SUBMISSION.tsv",
            _tsv(
                (
                    "ACCESSION_NUMBER",
                    "FILING_DATE",
                    "SUBMISSIONTYPE",
                    "CIK",
                    "PERIODOFREPORT",
                ),
                (
                    ("0000000001-23-000001", "14-Feb-2023", "13F-HR", "1", "31-Dec-2022"),
                    ("0000000001-23-000002", "20-Feb-2023", "13F-HR/A", "1", "31-Dec-2022"),
                    ("0000000001-23-000003", "25-Feb-2023", "13F-HR/A", "1", "31-Dec-2022"),
                ),
            ),
        )
        archive.writestr(
            f"{prefix}COVERPAGE.tsv",
            _tsv(
                (
                    "ACCESSION_NUMBER",
                    "REPORTCALENDARORQUARTER",
                    "ISAMENDMENT",
                    "AMENDMENTNO",
                    "AMENDMENTTYPE",
                    "FILINGMANAGER_NAME",
                    "FILINGMANAGER_CITY",
                    "FILINGMANAGER_STATEORCOUNTRY",
                    "REPORTTYPE",
                    "FORM13FFILENUMBER",
                    "CRDNUMBER",
                    "SECFILENUMBER",
                ),
                (
                    (
                        "0000000001-23-000001",
                        "31-Dec-2022",
                        "false",
                        "",
                        "",
                        "Example Capital",
                        "New York",
                        "NY",
                        "13F HOLDINGS REPORT",
                        "028-00001",
                        "",
                        "",
                    ),
                    (
                        "0000000001-23-000002",
                        "31-Dec-2022",
                        "true",
                        "1",
                        "RESTATEMENT",
                        "Example Capital",
                        "New York",
                        "NY",
                        "13F HOLDINGS REPORT",
                        "028-00001",
                        "",
                        "",
                    ),
                    (
                        "0000000001-23-000003",
                        "31-Dec-2022",
                        "true",
                        "2",
                        "ADD NEW HOLDINGS",
                        "Example Capital",
                        "New York",
                        "NY",
                        "13F HOLDINGS REPORT",
                        "028-00001",
                        "",
                        "",
                    ),
                ),
            ),
        )
        archive.writestr(
            f"{prefix}SUMMARYPAGE.tsv",
            _tsv(
                (
                    "ACCESSION_NUMBER",
                    "OTHERINCLUDEDMANAGERSCOUNT",
                    "TABLEENTRYTOTAL",
                    "TABLEVALUETOTAL",
                    "ISCONFIDENTIALOMITTED",
                ),
                (
                    ("0000000001-23-000001", 0, 1, 123, "false"),
                    ("0000000001-23-000002", 0, 1, 125, "false"),
                    ("0000000001-23-000003", 0, 1, 50, "false"),
                ),
            ),
        )
        archive.writestr(
            f"{prefix}INFOTABLE.tsv",
            _tsv(
                (
                    "ACCESSION_NUMBER",
                    "INFOTABLE_SK",
                    "NAMEOFISSUER",
                    "TITLEOFCLASS",
                    "CUSIP",
                    "FIGI",
                    "VALUE",
                    "SSHPRNAMT",
                    "SSHPRNAMTTYPE",
                    "PUTCALL",
                    "INVESTMENTDISCRETION",
                    "OTHERMANAGER",
                    "VOTING_AUTH_SOLE",
                    "VOTING_AUTH_SHARED",
                    "VOTING_AUTH_NONE",
                ),
                (
                    ("0000000001-23-000001", 1, "Issuer", "COM", "037833100", "", 123, 10, "SH", "", "SOLE", "", 10, 0, 0),
                    ("0000000001-23-000002", 2, "Issuer", "COM", "037833100", "", 125, 11, "SH", "", "SOLE", "", 11, 0, 0),
                    ("0000000001-23-000003", 3, "Other Issuer", "COM", "594918104", "", 50, 5, "SH", "", "SOLE", "", 5, 0, 0),
                ),
            ),
        )


def test_archive_name_parsing_and_selection() -> None:
    range_archive = archive_from_url(
        "https://www.sec.gov/files/structureddata/data/form-13f-data-sets/01mar2026-31may2026_form13f.zip"
    )
    quarter_archive = archive_from_url("https://www.sec.gov/files/2013q4_form13f.zip")

    assert (range_archive.period_start, range_archive.period_end) == (
        dt.date(2026, 3, 1),
        dt.date(2026, 5, 31),
    )
    assert (quarter_archive.period_start, quarter_archive.period_end) == (
        dt.date(2013, 10, 1),
        dt.date(2013, 12, 31),
    )
    assert select_archives(
        (range_archive, quarter_archive),
        start=dt.date(2026, 1, 1),
        end=dt.date(2026, 12, 31),
    ) == (range_archive,)
    assert archive_manifest_sha256((range_archive, quarter_archive)) == archive_manifest_sha256(
        (quarter_archive, range_archive)
    )


def test_load_archive_is_idempotent_and_normalizes_values(
    archive_store: DuckDBStore, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    archive = archive_from_url("https://www.sec.gov/files/2023q1_form13f.zip")
    zip_path = tmp_path / "2023q1_form13f.zip"
    _write_tiny_archive(zip_path)
    monkeypatch.setattr(
        "atx_db.thirteenf_archive.download_archive",
        lambda *_args, **_kwargs: zip_path,
    )
    options = ThirteenFArchiveBackfillOptions(
        cache_dir=tmp_path / "cache",
        extract_dir=tmp_path / "staging",
        run_id="test-run",
    )

    first = load_archive(archive_store, archive, options)
    second = load_archive(archive_store, archive, options)

    assert first.submission_rows == second.submission_rows == 3
    assert first.holding_rows == second.holding_rows == 3
    assert not (options.extract_dir / archive.source_period).exists()
    assert archive_store.con.execute(
        "SELECT count(*) FROM thirteenf_holdings WHERE source_period = ?", [archive.source_period]
    ).fetchone()[0] == 3
    assert archive_store.con.execute(
        "SELECT value_usd FROM thirteenf_holdings ORDER BY accession_number"
    ).fetchall() == [(123_000.0,), (125_000.0,), (50_000.0,)]
    assert archive_store.con.execute(
        "SELECT table_value_total FROM thirteenf_summary_pages ORDER BY accession_number"
    ).fetchall() == [(123_000.0,), (125_000.0,), (50_000.0,)]
    assert archive_store.con.execute(
        "SELECT count(*), min(status) FROM raw_source_files WHERE dataset_id = 'sec_13f_archive'"
    ).fetchone() == (1, "loaded")

    summary = archive_backfill_result((first,))
    assert summary.rows_loaded == 3
    assert summary.details["archive_count"] == 1

    from atx_db.thirteenf_amendments import refresh_thirteenf_amendments

    refresh = refresh_thirteenf_amendments(
        archive_store,
        minimum_history_quarters=2,
        run_id="test-amendments",
    )
    assert refresh.effective_position_rows == 2
    assert refresh.correction_rows == 2
    assert archive_store.con.execute(
        """
        SELECT cusip, value_usd, share_quantity
        FROM thirteenf_effective_positions
        ORDER BY cusip
        """
    ).fetchall() == [
        ("037833100", 125_000.0, 11.0),
        ("594918104", 50_000.0, 5.0),
    ]
    assert archive_store.con.execute(
        """
        SELECT amendment_type, change_type, cusip
        FROM thirteenf_amendment_corrections
        ORDER BY amendment_sequence
        """
    ).fetchall() == [
        ("RESTATEMENT", "CHANGED", "037833100"),
        ("ADD NEW HOLDINGS", "ADDED", "594918104"),
    ]
    assert archive_store.con.execute(
        """
        SELECT amendment_count, position_count, corrected_position_count, amendment_rate
        FROM thirteenf_amendment_rates
        """
    ).fetchone() == (2, 2, 2, 1.0)


def test_load_archive_accepts_period_directory_layout(
    archive_store: DuckDBStore, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    archive = archive_from_url(
        "https://www.sec.gov/files/structureddata/data/form-13f-data-sets/"
        "01jun2025-31aug2025_form13f.zip"
    )
    zip_path = tmp_path / "01jun2025-31aug2025_form13f.zip"
    _write_tiny_archive(zip_path, prefix="01JUN2025-31AUG2025_form13f/")
    monkeypatch.setattr(
        "atx_db.thirteenf_archive.download_archive",
        lambda *_args, **_kwargs: zip_path,
    )

    result = load_archive(
        archive_store,
        archive,
        ThirteenFArchiveBackfillOptions(extract_dir=tmp_path / "staging"),
    )

    assert result.holding_rows == 3


def test_fast_amendment_refresh_clears_stale_effective_positions(
    archive_store: DuckDBStore, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    archive = archive_from_url("https://www.sec.gov/files/2023q1_form13f.zip")
    zip_path = tmp_path / "2023q1_form13f.zip"
    _write_tiny_archive(zip_path)
    monkeypatch.setattr(
        "atx_db.thirteenf_archive.download_archive",
        lambda *_args, **_kwargs: zip_path,
    )
    load_archive(
        archive_store,
        archive,
        ThirteenFArchiveBackfillOptions(extract_dir=tmp_path / "staging"),
    )

    from atx_db.thirteenf_amendments import refresh_thirteenf_amendments

    refresh_thirteenf_amendments(archive_store, minimum_history_quarters=2)
    assert archive_store.con.execute(
        "SELECT count(*) FROM thirteenf_effective_positions"
    ).fetchone()[0] == 2

    refresh_thirteenf_amendments(
        archive_store,
        minimum_history_quarters=2,
        materialize_effective_positions=False,
    )
    assert archive_store.con.execute(
        "SELECT count(*) FROM thirteenf_effective_positions"
    ).fetchone()[0] == 0
