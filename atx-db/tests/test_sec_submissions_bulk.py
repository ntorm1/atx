from __future__ import annotations

import hashlib
import json
import logging
import zipfile
from dataclasses import replace
from pathlib import Path

import pandas as pd
import pytest

from atx_db import sec_submissions
from atx_db.connection import DuckDBStore
from atx_db.sec_submissions import SecSubmissionsBulkDataset, SecSubmissionsBulkOptions
from atx_db.warehouse import record_source_file


def _write_bulk_zip(path: Path) -> Path:
    main_one = {
        "cik": "1",
        "name": "Alpha Corp",
        "tickers": ["ALPH"],
        "filings": {
            "recent": {
                "accessionNumber": ["0000000001-24-000001", "0000000001-24-000002"],
                "filingDate": ["2024-02-01", "2024-03-01"],
                "reportDate": ["2023-12-31", ""],
                "acceptanceDateTime": [
                    "2024-02-01T16:30:00.000Z",
                    "2024-03-01T09:15:00.000Z",
                ],
                "act": ["34", "34"],
                "form": ["10-K", "4"],
                "fileNumber": ["001-00001", "001-00001"],
                "filmNumber": ["24000001", "24000002"],
                "items": ["", ""],
                "size": [1024, 256],
                "isXBRL": [1, 0],
                "isInlineXBRL": [1, 0],
                "primaryDocument": ["alpha-10k.htm", "form4.xml"],
                "primaryDocDescription": ["10-K", "FORM 4"],
            },
            "files": [
                {
                    "name": "CIK0000000001-submissions-001.json",
                    "filingCount": 1,
                    "filingFrom": "2014-01-01",
                    "filingTo": "2014-12-31",
                }
            ],
        },
    }
    # History members are columnar at the top level and omit primaryDocDescription.
    history_one = {
        "accessionNumber": ["0000000001-14-000001"],
        "filingDate": ["2014-05-01"],
        "reportDate": ["2014-03-31"],
        "acceptanceDateTime": ["2014-05-01T12:00:00.000Z"],
        "act": ["34"],
        "form": ["10-Q"],
        "fileNumber": ["001-00001"],
        "filmNumber": ["14000001"],
        "items": [""],
        "size": [512],
        "isXBRL": [1],
        "isInlineXBRL": [0],
        "primaryDocument": ["alpha-10q.htm"],
    }
    main_two = {
        "cik": "2",
        "name": "Beta Corp",
        "tickers": [],
        "filings": {
            "recent": {
                "accessionNumber": ["0000000002-24-000001"],
                "filingDate": ["2024-04-01"],
                "reportDate": [""],
                "acceptanceDateTime": ["2024-04-01T10:00:00.000Z"],
                "act": ["34"],
                "form": ["4"],
                "fileNumber": ["001-00002"],
                "filmNumber": ["24000003"],
                "items": [""],
                "size": [128],
                "isXBRL": [0],
                "isInlineXBRL": [0],
                "primaryDocument": ["form4.xml"],
                "primaryDocDescription": ["FORM 4"],
            },
            "files": [],
        },
    }
    zip_path = path / "submissions.zip"
    with zipfile.ZipFile(zip_path, "w") as archive:
        archive.writestr("CIK0000000001.json", json.dumps(main_one))
        archive.writestr("CIK0000000001-submissions-001.json", json.dumps(history_one))
        archive.writestr("CIK0000000002.json", json.dumps(main_two))
        archive.writestr("placeholder.txt", "not a submissions member")
    return zip_path


def test_bulk_load_reads_recent_and_history_with_form_filter(tmp_store, tmp_path) -> None:
    tmp_store.con.execute(
        """
        INSERT INTO sec_company_tickers (cik,ticker,title,security_id)
        VALUES ('0000000001','ALPH','ALPHA CORP','SEC-CIK-0000000001')
        """
    )
    zip_path = _write_bulk_zip(tmp_path)

    result = SecSubmissionsBulkDataset().load(
        tmp_store,
        SecSubmissionsBulkOptions(zip_path=zip_path, run_id="bulk-test-1"),
    )

    assert result.rows_loaded == 2
    rows = tmp_store.con.execute(
        """
        SELECT cik, accession_number, form, security_id, source_url, run_id,
               primary_doc_description, acceptance_datetime_raw
        FROM sec_submissions
        ORDER BY accession_number
        """
    ).fetchall()
    assert len(rows) == 2
    by_accession = {row[1]: row for row in rows}

    recent = by_accession["0000000001-24-000001"]
    assert recent[0] == "0000000001"
    assert recent[2] == "10-K"
    assert recent[3] == "SEC-CIK-0000000001"
    assert "CIK0000000001.json" in recent[4]
    assert recent[5] == "bulk-test-1"
    assert recent[6] == "10-K"
    assert recent[7] == "2024-02-01T16:30:00.000Z"

    history = by_accession["0000000001-14-000001"]
    assert history[2] == "10-Q"
    assert history[3] == "SEC-CIK-0000000001"
    assert "CIK0000000001-submissions-001.json" in history[4]
    assert history[6] is None
    assert history[7] == "2014-05-01T12:00:00.000Z"


def test_bulk_load_is_idempotent_on_replay(tmp_store, tmp_path) -> None:
    zip_path = _write_bulk_zip(tmp_path)
    options = SecSubmissionsBulkOptions(zip_path=zip_path, run_id="bulk-test-2")

    first = SecSubmissionsBulkDataset().load(tmp_store, options)
    second = SecSubmissionsBulkDataset().load(tmp_store, options)

    assert first.rows_loaded == 2
    assert second.rows_loaded == 2
    count = tmp_store.con.execute("SELECT count(*) FROM sec_submissions").fetchone()[0]
    assert count == 2


def test_bulk_load_supports_cik_scope_and_no_form_filter(tmp_store, tmp_path) -> None:
    zip_path = _write_bulk_zip(tmp_path)

    result = SecSubmissionsBulkDataset().load(
        tmp_store,
        SecSubmissionsBulkOptions(
            zip_path=zip_path,
            forms=None,
            ciks=("2",),
            run_id="bulk-test-3",
        ),
    )

    assert result.rows_loaded == 1
    rows = tmp_store.con.execute(
        "SELECT cik, form, security_id FROM sec_submissions"
    ).fetchall()
    assert rows == [("0000000002", "4", "SEC-CIK-0000000002")]


def _configure_bulk_session(store: DuckDBStore) -> None:
    store.analytical_memory_limit = "256MB"
    store.analytical_threads = 1
    store.con.execute("SET memory_limit = ?", [store.analytical_memory_limit])
    store.con.execute("SET threads = ?", [store.analytical_threads])
    store.con.execute("SET preserve_insertion_order = false")


def _bulk_session_settings(store: DuckDBStore):
    return store.con.execute(
        "SELECT current_setting('memory_limit'), current_setting('threads'), "
        "current_setting('preserve_insertion_order'), current_setting('TimeZone'), "
        "current_setting('temp_directory')"
    ).fetchone()


def test_bulk_recycle_preserves_rows_replacement_and_session_caps(
    tmp_store, tmp_path, monkeypatch, caplog
) -> None:
    _configure_bulk_session(tmp_store)
    settings = _bulk_session_settings(tmp_store)
    tmp_store.con.execute(
        """
        INSERT INTO sec_submissions (security_id,cik,accession_number,source_url,run_id)
        VALUES ('SEC-CIK-0000000009','0000000009','unrelated','prior-source','prior-run'),
               ('SEC-CIK-0000000001','0000000001','0000000001-24-000001','old-source','old-run')
        """
    )
    monkeypatch.setattr(sec_submissions, "_BULK_REOPEN_FLUSHES", 1)
    close = tmp_store.close
    committed_counts = []

    def checkpoint_and_close() -> None:
        committed_counts.append(
            tmp_store.con.execute("SELECT count(*) FROM sec_submissions").fetchone()[0]
        )
        # The real CHECKPOINT also rejects an active transaction.
        close()

    def unexpected_initialize() -> None:
        raise AssertionError("recycling must not initialize or migrate the warehouse")

    monkeypatch.setattr(tmp_store, "close", checkpoint_and_close)
    monkeypatch.setattr(tmp_store, "initialize", unexpected_initialize)
    zip_path = _write_bulk_zip(tmp_path)
    original_connection = tmp_store.con
    caplog.set_level(logging.INFO, logger=sec_submissions.__name__)

    for run_id in ("first", "replay"):
        result = SecSubmissionsBulkDataset().load(
            tmp_store,
            SecSubmissionsBulkOptions(
                zip_path=zip_path, forms=None, batch_ciks=1, run_id=run_id
            ),
        )
        assert result.rows_loaded == 4
        assert _bulk_session_settings(tmp_store) == settings

    assert tmp_store.con is not original_connection
    assert committed_counts == [4, 5, 5, 5]
    assert tmp_store.con.execute(
        "SELECT accession_number, form, run_id FROM sec_submissions ORDER BY accession_number"
    ).fetchall() == [
        ("0000000001-14-000001", "10-Q", "replay"),
        ("0000000001-24-000001", "10-K", "replay"),
        ("0000000001-24-000002", "4", "replay"),
        ("0000000002-24-000001", "4", "replay"),
        ("unrelated", None, "prior-run"),
    ]
    assert "main_members_processed=2/2 ciks_loaded=2 committed_rows=4 flushes=2" in caplog.text
    assert "connection reopened: committed_rows=4 flushes=2" in caplog.text
    assert "Alpha Corp" not in caplog.text
    assert str(zip_path) not in caplog.text


@pytest.mark.parametrize("memory_path", [":memory:", ":memory:submissions-bounded"])
def test_bulk_never_recycles_an_in_memory_store(
    tmp_store, tmp_path, monkeypatch, memory_path
) -> None:
    # Copy only the four table definitions needed by this loader, without
    # bootstrapping/migrating a second warehouse for the in-memory regression.
    definitions = tmp_store.con.execute(
        """
        SELECT sql FROM duckdb_tables()
        WHERE table_name IN (
            'sec_company_tickers', 'sec_submissions', 'raw_source_files', 'data_quality_checks'
        ) AND NOT temporary
        """
    ).fetchall()
    assert len(definitions) == 4
    monkeypatch.setattr(DuckDBStore, "initialize", lambda _store: None)
    monkeypatch.setattr(sec_submissions, "_BULK_REOPEN_FLUSHES", 1)
    with DuckDBStore(memory_path) as store:
        for (definition,) in definitions:
            store.con.execute(definition)
        _configure_bulk_session(store)
        original_connection = store.con

        def unexpected_close() -> None:
            raise AssertionError("an in-memory warehouse must not be closed")

        monkeypatch.setattr(store, "close", unexpected_close)
        options = SecSubmissionsBulkOptions(
            zip_path=_write_bulk_zip(tmp_path), forms=None, batch_ciks=1
        )
        for _ in range(2):
            assert SecSubmissionsBulkDataset().load(store, options).rows_loaded == 4
        assert store.con is original_connection
        assert store.con.execute("SELECT count(*) FROM sec_submissions").fetchone() == (4,)


@pytest.mark.parametrize("temporary_kind", ["registered", "table", "unconfigured"])
def test_bulk_preserves_caller_session_state(
    tmp_store, tmp_path, monkeypatch, temporary_kind
) -> None:
    if temporary_kind != "unconfigured":
        _configure_bulk_session(tmp_store)
    if temporary_kind == "registered":
        tmp_store.con.register("caller_rows", pd.DataFrame({"value": [17]}))
    elif temporary_kind == "table":
        tmp_store.con.execute("CREATE TEMP TABLE caller_rows AS SELECT 17 AS value")
    monkeypatch.setattr(sec_submissions, "_BULK_REOPEN_FLUSHES", 1)

    def unexpected_close() -> None:
        raise AssertionError("caller-owned session state must prevent recycling")

    monkeypatch.setattr(tmp_store, "close", unexpected_close)
    original_connection = tmp_store.con
    settings = _bulk_session_settings(tmp_store)
    result = SecSubmissionsBulkDataset().load(
        tmp_store,
        SecSubmissionsBulkOptions(
            zip_path=_write_bulk_zip(tmp_path), forms=None, batch_ciks=1
        ),
    )
    assert result.rows_loaded == 4
    assert tmp_store.con is original_connection
    assert _bulk_session_settings(tmp_store) == settings
    assert tmp_store.con.execute("SELECT count(*) FROM sec_submissions").fetchone() == (4,)
    if temporary_kind != "unconfigured":
        assert tmp_store.con.execute("SELECT value FROM caller_rows").fetchall() == [(17,)]


def test_bulk_logs_progress_when_form_filter_leaves_no_rows(
    tmp_store, tmp_path, monkeypatch, caplog
) -> None:
    monkeypatch.setattr(sec_submissions, "_BULK_PROGRESS_MEMBERS", 1)
    caplog.set_level(logging.INFO, logger=sec_submissions.__name__)
    result = SecSubmissionsBulkDataset().load(
        tmp_store,
        SecSubmissionsBulkOptions(zip_path=_write_bulk_zip(tmp_path), forms=()),
    )
    assert result.rows_loaded == 0
    assert "main_members_processed=1/2 ciks_loaded=0 committed_rows=0 flushes=0" in caplog.text
    assert "main_members_processed=2/2 ciks_loaded=0 committed_rows=0 flushes=0" in caplog.text


def test_raw_acceptance_and_archive_metadata_survive_failed_prefix_resume(
    tmp_store, tmp_path, monkeypatch
) -> None:
    zip_path = _write_bulk_zip(tmp_path)
    archive_sha = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    record_source_file(
        tmp_store, dataset_id="sec_submissions", source_url="https://sec.example/submissions.zip",
        cache_path=zip_path, compute_hash=True,
    )
    options = SecSubmissionsBulkOptions(zip_path=zip_path, forms=None, batch_ciks=1)
    original = sec_submissions._replace_submission_rows
    calls = 0

    def interrupt_second_batch(store, frame):
        nonlocal calls
        calls += 1
        if calls == 2:
            raise RuntimeError("fixture interruption")
        return original(store, frame)

    with monkeypatch.context() as patch:
        patch.setattr(sec_submissions, "_replace_submission_rows", interrupt_second_batch)
        with pytest.raises(RuntimeError, match="fixture interruption"):
            SecSubmissionsBulkDataset().run(tmp_store, options)

    prior_id = tmp_store.con.execute(
        "SELECT run_id FROM dataset_runs WHERE dataset_id = 'sec_submissions' ORDER BY started_at DESC LIMIT 1"
    ).fetchone()[0]
    prefix = tmp_store.con.execute(
        "SELECT cik, accession_number, form, acceptance_datetime_raw FROM sec_submissions ORDER BY accession_number"
    ).fetchall()
    assert len(prefix) == 3
    assert [row[3] for row in prefix] == [
        "2014-05-01T12:00:00.000Z", "2024-02-01T16:30:00.000Z", "2024-03-01T09:15:00.000Z",
    ]

    result = SecSubmissionsBulkDataset().run(
        tmp_store, replace(options, resume_from_run_id=prior_id)
    )
    assert result.rows_loaded == 1
    assert result.details["verified_prior_rows"] == 3
    assert result.details["resume_after_cik"] == "0000000001"
    assert result.details["archive_sha256"] == archive_sha
    assert result.details["scope_complete"] is True
    after = tmp_store.con.execute(
        "SELECT cik, accession_number, form, acceptance_datetime_raw FROM sec_submissions ORDER BY accession_number"
    ).fetchall()
    assert after[:3] == prefix
    assert after[3] == ("0000000002", "0000000002-24-000001", "4", "2024-04-01T10:00:00.000Z")
    receipt = json.loads(tmp_store.con.execute(
        "SELECT metadata_json FROM raw_source_files WHERE source_url = ?", [str(zip_path)]
    ).fetchone()[0])
    assert receipt["archive_sha256"] == archive_sha
    assert receipt["covered_rows"] == 4
