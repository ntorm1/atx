"""Activation stages: SEC bulk download, submissions load, companyfacts load."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import zipfile
from pathlib import Path

import pytest

from atx_db.activation import (
    COMPANYFACTS_ZIP_URL,
    SUBMISSIONS_ZIP_URL,
    ActivationOptions,
    ActivationStageError,
    run_activation,
    sha256_file,
    stage_companyfacts_load,
    stage_sec_bulk_download,
    stage_submissions_load,
)
from atx_db.sec_http import APPROVED_SEC_USER_AGENT

_CIK = "0000320193"


def _companyfacts_payload() -> bytes:
    return json.dumps(
        {
            "cik": 320193,
            "entityName": "Apple Inc.",
            "facts": {
                "us-gaap": {
                    "Assets": {
                        "label": "Assets",
                        "description": "Total assets",
                        "units": {
                            "USD": [
                                {
                                    "end": "2023-09-30",
                                    "val": 352583000000,
                                    "accn": "0000320193-23-000106",
                                    "fy": 2023,
                                    "fp": "FY",
                                    "form": "10-K",
                                    "filed": "2023-11-03",
                                    "frame": "CY2023Q3I",
                                }
                            ]
                        },
                    }
                }
            },
        }
    ).encode("utf-8")


def _submissions_payload() -> bytes:
    return json.dumps(
        {
            "cik": _CIK,
            "name": "Apple Inc.",
            "filings": {
                "recent": {
                    "accessionNumber": ["0000320193-23-000106"],
                    "filingDate": ["2023-11-03"],
                    "reportDate": ["2023-09-30"],
                    "acceptanceDateTime": ["2023-11-02T18:08:27.000Z"],
                    "form": ["10-K"],
                    "primaryDocument": ["aapl-20230930.htm"],
                    "primaryDocDescription": ["10-K"],
                },
                "files": [],
            },
        }
    ).encode("utf-8")


@pytest.fixture
def companyfacts_zip(tmp_path: Path) -> Path:
    path = tmp_path / "fixtures" / "companyfacts.zip"
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(f"CIK{_CIK}.json", _companyfacts_payload())
    return path


@pytest.fixture
def submissions_zip(tmp_path: Path) -> Path:
    path = tmp_path / "fixtures" / "submissions.zip"
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(f"CIK{_CIK}.json", _submissions_payload())
    return path


def _seed_company_ticker(store) -> None:
    # sec_company_tickers has no "source" column (schema.py: cik, ticker, title,
    # security_id, source_loaded_at) -- the brief's insert referenced a stale
    # column name; corrected here to match the real schema.
    store.con.execute(
        """
        INSERT INTO sec_company_tickers (cik, ticker, title, security_id, source_loaded_at)
        VALUES (?, 'AAPL', 'Apple Inc.', 'SEC-CIK-0000320193', TIMESTAMP '2024-01-01 00:00:00')
        """,
        [_CIK],
    )


def _options(tmp_path: Path, downloader=None) -> ActivationOptions:
    return ActivationOptions(
        companyfacts_mode="legacy",
        submissions_mode="legacy",
        db_path=tmp_path / "warehouse.duckdb",
        as_of_date=dt.date(2024, 1, 4),
        staging_dir=tmp_path / "staging",
        cache_dir=tmp_path / "cache",
        sec_user_agent=APPROVED_SEC_USER_AGENT,
        downloader=downloader,
        ticker_history_expected_bytes=None,
        run_id="test-run",
    )


def test_sha256_file_matches_hashlib(tmp_path):
    path = tmp_path / "x.bin"
    path.write_bytes(b"hello world")
    assert sha256_file(path) == hashlib.sha256(b"hello world").hexdigest()


def test_download_stage_fetches_both_archives_and_records_hashes(
    tmp_store, tmp_path, companyfacts_zip, submissions_zip
):
    requested: list[str] = []

    def fake_downloader(url: str, dest: Path, *, user_agent: str) -> int:
        requested.append(url)
        source = companyfacts_zip if url == COMPANYFACTS_ZIP_URL else submissions_zip
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(source.read_bytes())
        return dest.stat().st_size

    options = _options(tmp_path, fake_downloader)
    result = stage_sec_bulk_download(tmp_store, options)
    assert requested == [COMPANYFACTS_ZIP_URL, SUBMISSIONS_ZIP_URL]
    assert options.companyfacts_zip.is_file()
    assert options.submissions_zip.is_file()
    assert result.detail["companyfacts_sha256"] == sha256_file(companyfacts_zip)
    assert result.detail["submissions_sha256"] == sha256_file(submissions_zip)
    assert result.rows == 2
    rows = tmp_store.con.execute(
        "SELECT dataset_id, sha256 FROM raw_source_files WHERE dataset_id IN ('sec_company_facts', 'sec_submissions')"
    ).fetchall()
    recorded = dict(rows)
    assert recorded["sec_company_facts"] == sha256_file(companyfacts_zip)
    assert recorded["sec_submissions"] == sha256_file(submissions_zip)


def test_download_stage_resumes_by_skipping_present_archives(
    tmp_store, tmp_path, companyfacts_zip, submissions_zip
):
    calls: list[str] = []

    def fake_downloader(url: str, dest: Path, *, user_agent: str) -> int:
        calls.append(url)
        source = companyfacts_zip if url == COMPANYFACTS_ZIP_URL else submissions_zip
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(source.read_bytes())
        return dest.stat().st_size

    options = _options(tmp_path, fake_downloader)
    stage_sec_bulk_download(tmp_store, options)
    result = stage_sec_bulk_download(tmp_store, options)
    assert calls == [COMPANYFACTS_ZIP_URL, SUBMISSIONS_ZIP_URL]
    assert result.detail["companyfacts_skipped"] is True
    assert result.detail["submissions_skipped"] is True


def test_download_stage_refuses_a_non_approved_sec_user_agent(tmp_store, tmp_path, monkeypatch):
    monkeypatch.delenv("ATX_SEC_USER_AGENT", raising=False)
    options = ActivationOptions(**{**_options(tmp_path).as_dict(), "sec_user_agent": "atx-db/0.2 other-agent"})
    with pytest.raises(ValueError, match="ATX_SEC_USER_AGENT"):
        stage_sec_bulk_download(tmp_store, options)


def test_submissions_stage_loads_the_bulk_archive(tmp_store, tmp_path, submissions_zip):
    _seed_company_ticker(tmp_store)
    options = _options(tmp_path)
    options.submissions_zip.parent.mkdir(parents=True, exist_ok=True)
    options.submissions_zip.write_bytes(submissions_zip.read_bytes())
    result = stage_submissions_load(tmp_store, options)
    assert result.rows == 1
    row = tmp_store.con.execute(
        "SELECT count(*) FROM sec_submissions WHERE accession_number = '0000320193-23-000106'"
    ).fetchone()
    assert row is not None and row[0] == 1


def test_submissions_stage_is_idempotent(tmp_store, tmp_path, submissions_zip):
    _seed_company_ticker(tmp_store)
    options = _options(tmp_path)
    options.submissions_zip.parent.mkdir(parents=True, exist_ok=True)
    options.submissions_zip.write_bytes(submissions_zip.read_bytes())
    stage_submissions_load(tmp_store, options)
    stage_submissions_load(tmp_store, options)
    row = tmp_store.con.execute("SELECT count(*) FROM sec_submissions").fetchone()
    assert row is not None and row[0] == 1


def test_staged_submissions_refuses_legacy_resume_before_opening_store(tmp_path):
    options = ActivationOptions(**{**_options(tmp_path).as_dict(), "submissions_mode": "staged",
                                   "submissions_resume_from_run_id": "old-loader-uuid"})
    with pytest.raises(ValueError, match="build ledger"):
        stage_submissions_load(None, options)


def test_staged_submissions_requires_nested_guard(tmp_path, monkeypatch):
    monkeypatch.delenv("ATX_GUARD_JOB", raising=False)
    options = ActivationOptions(**{**_options(tmp_path).as_dict(), "submissions_mode": "staged"})
    with pytest.raises(ValueError, match="Submissions.*non-HEAVY.*allow-nested-guards"):
        stage_submissions_load(None, options)


def test_staged_submissions_delegates_only_after_store_close(tmp_path, monkeypatch):
    import json
    from types import SimpleNamespace
    from atx_db import activation, submissions_rebuild

    state = {"closed": False, "reopened": False}
    store = SimpleNamespace(
        close=lambda: state.update(closed=True), reopen=lambda: state.update(reopened=True),
        con=SimpleNamespace(execute=lambda *args: SimpleNamespace(
            fetchone=lambda: ("published", json.dumps({"source_rows": 12})))),
    )

    def run(args):
        assert state["closed"] and not state["reopened"]
        assert args[args.index("--stage")+1] == submissions_rebuild.STAGE_NAME
        for flag, value in (("--job-gb", "0.8"), ("--max-batches", "1"),
                            ("--max-minutes", "1"), ("--duckdb-memory", "384MB"), ("--threads", "1")):
            assert args[args.index(flag)+1] == value
        assert "--heavy" in args and "--finalize" in args
        return 0

    monkeypatch.setattr(activation, "_companyfacts_parent_guard", lambda: {"verified_parent": True})
    monkeypatch.setattr(activation.importlib.util, "spec_from_file_location",
                        lambda *args: SimpleNamespace(loader=SimpleNamespace(exec_module=lambda module: None)))
    monkeypatch.setattr(activation.importlib.util, "module_from_spec", lambda spec: SimpleNamespace(main=run))
    options = ActivationOptions(**{**_options(tmp_path).as_dict(), "submissions_mode": "staged",
        "submissions_staging_dir": tmp_path / "submissions", "submissions_receipts_dir": tmp_path / "receipts"})
    result = stage_submissions_load(store, options)
    assert result.rows == 12 and state["reopened"]


def test_companyfacts_stage_targets_all_sec_company_tickers_ciks(tmp_store, tmp_path, companyfacts_zip):
    _seed_company_ticker(tmp_store)
    options = _options(tmp_path)
    options.companyfacts_zip.parent.mkdir(parents=True, exist_ok=True)
    options.companyfacts_zip.write_bytes(companyfacts_zip.read_bytes())
    result = stage_companyfacts_load(tmp_store, options)
    assert result.rows >= 1
    assert result.detail["symbol_source"] == "sec_company_tickers"
    assert result.detail["skip_loaded"] is True
    row = tmp_store.con.execute(
        "SELECT count(*) FROM sec_company_facts WHERE cik = ?", [_CIK]
    ).fetchone()
    assert row is not None and row[0] >= 1


def test_companyfacts_stage_is_idempotent_with_skip_loaded(tmp_store, tmp_path, companyfacts_zip):
    _seed_company_ticker(tmp_store)
    options = _options(tmp_path)
    options.companyfacts_zip.parent.mkdir(parents=True, exist_ok=True)
    options.companyfacts_zip.write_bytes(companyfacts_zip.read_bytes())
    stage_companyfacts_load(tmp_store, options)
    before = tmp_store.con.execute("SELECT count(*) FROM sec_company_facts").fetchone()
    stage_companyfacts_load(tmp_store, options)
    after = tmp_store.con.execute("SELECT count(*) FROM sec_company_facts").fetchone()
    assert before == after


def test_companyfacts_stage_fails_without_the_archive(tmp_store, tmp_path):
    with pytest.raises(FileNotFoundError):
        stage_companyfacts_load(tmp_store, _options(tmp_path))


def test_companyfacts_stage_detail_keys_match_across_load_and_noop_runs(tmp_store, tmp_path, companyfacts_zip):
    _seed_company_ticker(tmp_store)
    options = _options(tmp_path)
    options.companyfacts_zip.parent.mkdir(parents=True, exist_ok=True)
    options.companyfacts_zip.write_bytes(companyfacts_zip.read_bytes())
    first_run = stage_companyfacts_load(tmp_store, options)
    second_run = stage_companyfacts_load(tmp_store, options)
    assert first_run.rows >= 1
    assert second_run.rows == 0
    assert set(first_run.detail) == set(second_run.detail)


def test_archive_stage_failure_keeps_partial_rows_and_durable_details(tmp_store, tmp_path, companyfacts_zip):
    options = ActivationOptions(**{**_options(tmp_path).as_dict(), "companyfacts_symbol_source": "archive_members",
                                   "db_path": tmp_store.path})
    options.companyfacts_zip.parent.mkdir(parents=True, exist_ok=True)
    options.companyfacts_zip.write_bytes(companyfacts_zip.read_bytes())
    with zipfile.ZipFile(options.companyfacts_zip, "a") as archive:
        archive.writestr("CIK0000000001.json", "broken JSON")
    emitted = []
    # The ladder opens its own store on this file; DuckDB refuses a second in-process
    # connection whose configuration differs from conftest's budgeted one (7d0b7ee9).
    tmp_store.close()
    with pytest.raises(ActivationStageError) as caught:
        run_activation(options, stages=("companyfacts_load",), emit=emitted.append)
    tmp_store.reopen()
    result = caught.value.result
    assert result.rows == 1
    assert result.detail["skip_loaded"] is False
    assert result.detail["failed_target_count"] == 1
    assert result.detail["completed_targets"] == 1
    assert result.detail["unresolved_security_targets"] == 1
    assert emitted[-1]["status"] == "failed"
    assert emitted[-1]["rows"] == 1
    assert emitted[-1]["detail"]["failed_target_count"] == 1
    row = tmp_store.con.execute(
        "SELECT status,\"rows\",error FROM activation_stage_runs WHERE stage='companyfacts_load' AND run_id='test-run'"
    ).fetchone()
    assert row[0:2] == ("failed", 1)
    assert '"failed_target_count": 1' in row[2]
    assert tmp_store.con.execute("SELECT count(*) FROM sec_company_facts").fetchone()[0] == 1
