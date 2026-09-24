from __future__ import annotations

import argparse
import json
import sqlite3
import zipfile
from dataclasses import replace
from pathlib import Path

import pytest

from atx_db import activation, sec_submissions
from atx_db._submissions_archive import SubmissionsArchive
from atx_db.sec_submissions import SecSubmissionsBulkDataset, SecSubmissionsBulkOptions
from atx_db.warehouse import record_source_file


def _columnar(accessions, forms):
    return {
        "accessionNumber": accessions,
        "form": forms,
        "filingDate": ["2024-01-01"] * len(accessions),
        "reportDate": [""] * len(accessions),
        "acceptanceDateTime": [""] * len(accessions),
        "primaryDocument": ["filing.htm"] * len(accessions),
        "primaryDocDescription": [""] * len(accessions),
    }


def _archive(path: Path, *, missing_history=False) -> Path:
    archive_path = path / "submissions.zip"
    with zipfile.ZipFile(archive_path, "w") as archive:
        for cik in range(1, 8):
            recent = _columnar([], []) if cik in (2, 5) else _columnar([f"{cik}-a"], ["4"])
            files = []
            if cik == 1:
                recent = _columnar([" 1-a ", "1-b"], [" 4 ", "10-K"])
                files = [{"name": "CIK0000000001-submissions-001.json"}]
                if missing_history:
                    files.append({"name": "CIK0000000001-submissions-missing.json"})
            archive.writestr(f"CIK{cik:010d}.json", json.dumps({"filings": {"recent": recent, "files": files}}))
        # A duplicate history accession must retain the first recent member's
        # form and source after stripping, exactly as the regular bulk loader.
        archive.writestr("CIK0000000001-submissions-001.json",
                         json.dumps(_columnar(["1-a", "1-old"], ["8-K", "13F-HR"])))
    return archive_path


def _stop_after_batches(store, options, monkeypatch, *, batches=1):
    replace_rows = sec_submissions._replace_submission_rows
    committed = 0

    def interrupt(store, frame):
        nonlocal committed
        if committed == batches:
            raise RuntimeError("fixture interruption before commit")
        rows = replace_rows(store, frame)
        committed += 1
        return rows

    with monkeypatch.context() as patch:
        patch.setattr(sec_submissions, "_replace_submission_rows", interrupt)
        with pytest.raises(RuntimeError, match="fixture interruption"):
            SecSubmissionsBulkDataset().run(store, options)
    return store.con.execute(
        "SELECT run_id FROM dataset_runs WHERE dataset_id = 'sec_submissions' ORDER BY started_at DESC LIMIT 1"
    ).fetchone()[0]


@pytest.fixture
def interrupted_bulk(tmp_store, tmp_path, monkeypatch):
    options = SecSubmissionsBulkOptions(zip_path=_archive(tmp_path), forms=None, batch_ciks=2)
    record_source_file(tmp_store, dataset_id="sec_submissions", source_url="https://sec.example/submissions.zip",
                       cache_path=options.zip_path, compute_hash=True)
    prior_id = _stop_after_batches(tmp_store, options, monkeypatch)
    return tmp_store, replace(options, resume_from_run_id=prior_id)


def _filings(store):
    return store.con.execute(
        "SELECT cik, accession_number, form, source_url, run_id FROM sec_submissions ORDER BY cik, accession_number"
    ).fetchall()


def test_verified_resume_keeps_full_scope_history_empty_members_and_lineage(interrupted_bulk, monkeypatch):
    store, options = interrupted_bulk
    before = _filings(store)
    assert len(before) == 4
    writes = []
    replace_rows = sec_submissions._replace_submission_rows

    def capture(store, frame):
        writes.extend(frame.cik.unique())
        return replace_rows(store, frame)

    monkeypatch.setattr(sec_submissions, "_replace_submission_rows", capture)
    result = SecSubmissionsBulkDataset().run(store, options)
    assert result.rows_loaded == 3
    assert writes == ["0000000004", "0000000006", "0000000007"]
    assert _filings(store)[:4] == before
    assert result.details["main_members"] == result.details["main_members_processed"] == 7
    assert result.details["verified_prior_rows"] == 4
    assert result.details["verified_prior_main_members"] == 3
    assert result.details["covered_rows"] == 7
    assert result.details["ciks_loaded"] == 5
    assert result.details["history_members_read"] == 1
    assert result.details["resume_after_cik"] == "0000000003"
    assert result.details["verified_prior_run_ids"] == (options.resume_from_run_id,)
    assert result.details["scope_complete"] is True
    assert store.con.execute("SELECT rows_loaded FROM dataset_runs WHERE run_id = ?", [result.run_id]).fetchone() == (3,)


def test_second_interruption_can_resume_verified_ancestry(interrupted_bulk, monkeypatch):
    store, options = interrupted_bulk
    successor_id = _stop_after_batches(store, options, monkeypatch)
    result = SecSubmissionsBulkDataset().run(store, replace(options, resume_from_run_id=successor_id))
    assert result.rows_loaded == 1
    assert result.details["verified_prior_rows"] == 6
    assert result.details["verified_prior_main_members"] == 6
    assert result.details["verified_prior_run_ids"] == (options.resume_from_run_id, successor_id)
    assert result.details["covered_rows"] == len(_filings(store)) == 7


def test_resume_rebuilds_corrupt_directory_without_skipping_issuer(interrupted_bulk):
    store, options = interrupted_bulk
    before = _filings(store)
    with SubmissionsArchive(options.zip_path) as archive:
        index_path = archive.index_path
    connection = sqlite3.connect(index_path)
    try:
        # Corrupt both a retained-prefix issuer and an unprocessed issuer. A
        # cached denominator must not authorize either an unverified prefix or
        # a narrowed successor scope, even when the SQLite file remains valid.
        connection.execute("DELETE FROM members WHERE name IN ('CIK0000000001.json', 'CIK0000000006.json')")
        connection.commit()
    finally:
        connection.close()
    result = SecSubmissionsBulkDataset().run(store, options)
    assert _filings(store)[:4] == before
    assert result.details["main_members"] == result.details["main_members_processed"] == 7
    assert result.details["verified_prior_rows"] == 4
    assert result.details["covered_rows"] == len(_filings(store)) == 7
    assert result.details["scope_complete"] is True


def test_uncommitted_batch_is_rolled_back_and_replayed(interrupted_bulk, monkeypatch):
    store, options = interrupted_bulk
    before = _filings(store)
    insert_frame = sec_submissions.insert_frame

    def interrupt_after_insert(*args, **kwargs):
        insert_frame(*args, **kwargs)
        raise RuntimeError("fixture interruption inside transaction")

    with monkeypatch.context() as patch:
        patch.setattr(sec_submissions, "insert_frame", interrupt_after_insert)
        with pytest.raises(RuntimeError, match="inside transaction"):
            SecSubmissionsBulkDataset().run(store, options)
    assert _filings(store) == before
    successor_id = store.con.execute(
        "SELECT run_id FROM dataset_runs WHERE dataset_id = 'sec_submissions' ORDER BY started_at DESC LIMIT 1"
    ).fetchone()[0]
    result = SecSubmissionsBulkDataset().run(store, replace(options, resume_from_run_id=successor_id))
    assert result.rows_loaded == 3
    assert result.details["covered_rows"] == len(_filings(store)) == 7


def test_resume_refuses_missing_referenced_prefix_history(tmp_store, tmp_path, monkeypatch):
    options = SecSubmissionsBulkOptions(zip_path=_archive(tmp_path, missing_history=True), forms=None, batch_ciks=2)
    record_source_file(tmp_store, dataset_id="sec_submissions", source_url="https://sec.example/submissions.zip",
                       cache_path=options.zip_path, compute_hash=True)
    prior_id = _stop_after_batches(tmp_store, options, monkeypatch)
    before = _filings(tmp_store)
    with pytest.raises(ValueError, match="missing referenced history"):
        SecSubmissionsBulkDataset().run(tmp_store, replace(options, resume_from_run_id=prior_id))
    assert _filings(tmp_store) == before


@pytest.mark.parametrize("corruption, message", [
    ("DELETE FROM sec_submissions WHERE accession_number = '1-old'", "prefix rows do not match"),
    ("UPDATE sec_submissions SET accession_number = '1-missing' WHERE accession_number = '1-old'", "prefix rows do not match"),
    ("UPDATE sec_submissions SET form = '10-Q' WHERE accession_number = '1-old'", "prefix rows do not match"),
    ("UPDATE sec_submissions SET source_url = 'wrong' WHERE accession_number = '1-old'", "prefix rows do not match"),
    ("UPDATE sec_submissions SET security_id = 'wrong' WHERE accession_number = '1-old'", "prefix rows do not match"),
    ("DELETE FROM sec_submissions WHERE cik = '0000000001'", "complete batches"),
    # Same counts and maximum CIK cannot establish a complete archive prefix.
    ("UPDATE sec_submissions SET cik = '0000000002' WHERE cik = '0000000001'", "prefix rows do not match"),
])
def test_resume_refuses_corrupt_prefix_without_writes(interrupted_bulk, corruption, message):
    store, options = interrupted_bulk
    store.con.execute(corruption)
    before = _filings(store)
    with pytest.raises(ValueError, match=message):
        SecSubmissionsBulkDataset().run(store, options)
    assert _filings(store) == before


@pytest.mark.parametrize("change", [
    {"forms": ["10-K"]}, {"ciks": ["1", "3"]}, {"include_history_files": False},
    {"batch_ciks": 1}, {"zip_path": "another/submissions.zip"},
])
def test_resume_refuses_mismatched_prior_scope(interrupted_bulk, change):
    store, options = interrupted_bulk
    params = json.loads(store.con.execute(
        "SELECT params_json FROM dataset_runs WHERE run_id = ?", [options.resume_from_run_id],
    ).fetchone()[0])
    params.update(change)
    store.con.execute("UPDATE dataset_runs SET params_json = ? WHERE run_id = ?",
                      [json.dumps(params), options.resume_from_run_id])
    before = _filings(store)
    with pytest.raises(ValueError, match="scope does not match"):
        SecSubmissionsBulkDataset().run(store, options)
    assert _filings(store) == before


@pytest.mark.parametrize("change", [
    {"forms": ("10-K",)}, {"ciks": ("1",)}, {"include_history_files": False},
])
def test_resume_refuses_partial_requested_scope(interrupted_bulk, change):
    store, options = interrupted_bulk
    before = _filings(store)
    with pytest.raises(ValueError, match="all forms, all CIKs and history"):
        SecSubmissionsBulkDataset().run(store, replace(options, **change))
    assert _filings(store) == before


@pytest.mark.parametrize("change", ["archive", "receipt", "late_receipt", "running", "cycle"])
def test_resume_requires_hash_receipt_and_terminal_acyclic_lineage(interrupted_bulk, change):
    store, options = interrupted_bulk
    if change == "archive":
        with zipfile.ZipFile(options.zip_path, "a") as archive:
            archive.writestr("changed.txt", "different archive")
    elif change == "receipt":
        store.con.execute("DELETE FROM raw_source_files")
    elif change == "late_receipt":
        store.con.execute("UPDATE raw_source_files SET fetched_at = current_timestamp")
    elif change == "running":
        store.con.execute("UPDATE dataset_runs SET status = 'running' WHERE run_id = ?", [options.resume_from_run_id])
    else:
        params = json.loads(store.con.execute(
            "SELECT params_json FROM dataset_runs WHERE run_id = ?", [options.resume_from_run_id],
        ).fetchone()[0])
        params["resume_from_run_id"] = options.resume_from_run_id
        store.con.execute("UPDATE dataset_runs SET params_json = ? WHERE run_id = ?",
                          [json.dumps(params), options.resume_from_run_id])
    before = _filings(store)
    with pytest.raises(ValueError, match=r"receipt|terminal failed|cyclic lineage"):
        SecSubmissionsBulkDataset().run(store, options)
    assert _filings(store) == before


def test_activation_wires_explicit_resume_and_refuses_missing_history(tmp_path, monkeypatch):
    parser = argparse.ArgumentParser()
    activation.add_activation_arguments(parser)
    options = activation.activation_options_from_args(parser.parse_args([
        "--cache-dir", str(tmp_path), "--submissions-resume-from-run-id", "failed-dataset-uuid",
    ]))
    options.submissions_zip.write_bytes(b"fixture")
    seen = []

    def run(self, store, supplied):
        seen.append(supplied)
        return sec_submissions.DatasetLoadResult("sec_submissions", 0, "fixture", {"missing_history_members": 1})

    monkeypatch.setattr(SecSubmissionsBulkDataset, "run", run)
    with pytest.raises(ValueError, match="full stage is incomplete"):
        activation.stage_submissions_load(None, options)
    assert seen[0].resume_from_run_id == "failed-dataset-uuid"
    assert seen[0].forms is None and seen[0].ciks is None and seen[0].include_history_files
    assert seen[0].batch_ciks == 50
    assert (options.memory_limit, options.threads) == ("1GB", 1)


@pytest.mark.parametrize("change", [
    {"forms": ("10-K",)}, {"forms": ()}, {"include_history_files": False}, {"ciks": ("1",)},
])
def test_partial_load_never_claims_full_scope_complete(tmp_store, tmp_path, change):
    options = SecSubmissionsBulkOptions(zip_path=_archive(tmp_path), forms=None, batch_ciks=2)
    result = SecSubmissionsBulkDataset().load(tmp_store, replace(options, **change))
    assert result.details["scope_complete"] is False
    metadata = json.loads(tmp_store.con.execute(
        "SELECT metadata_json FROM raw_source_files WHERE source_url = ?", [str(options.zip_path)],
    ).fetchone()[0])
    assert metadata["scope_complete"] is False


@pytest.mark.parametrize("entrypoint", ["direct", "cli"])
def test_resume_plan_refused_before_database_or_download_side_effects(interrupted_bulk, monkeypatch, entrypoint):
    from atx_db import migration_admin

    store, resume_options = interrupted_bulk
    receipt_before = store.con.execute("SELECT * FROM raw_source_files ORDER BY source_id").fetchall()

    def forbidden(*args, **kwargs):
        pytest.fail("resume preflight must precede database, migration, and download work")

    monkeypatch.setattr(activation, "DuckDBStore", forbidden)
    monkeypatch.setattr(migration_admin, "pending_migrations", forbidden)
    monkeypatch.setitem(activation.STAGES, "sec_bulk_download", forbidden)
    with pytest.raises(ValueError, match=r"--only submissions_load.*--start-stage submissions_load"):
        if entrypoint == "direct":
            activation.run_activation(activation.ActivationOptions(
                db_path=store.path, cache_dir=resume_options.zip_path.parent, force=True,
                submissions_resume_from_run_id=resume_options.resume_from_run_id,
            ))
        else:
            parser = argparse.ArgumentParser()
            activation.add_activation_arguments(parser)
            args = parser.parse_args([
                "--db-path", str(store.path), "--cache-dir", str(resume_options.zip_path.parent),
                "--force", "--submissions-resume-from-run-id", resume_options.resume_from_run_id,
            ])
            activation.run_activation_from_args(args, governed_migrations=forbidden, run_activation=forbidden)
    assert store.con.execute("SELECT * FROM raw_source_files ORDER BY source_id").fetchall() == receipt_before
    # The rejected broad restart must leave the exact pre-attempt receipt usable.
    assert SecSubmissionsBulkDataset().run(store, resume_options).details["scope_complete"] is True


@pytest.mark.parametrize("stage_args", [["--only", "submissions_load"], ["--start-stage", "submissions_load"]])
def test_resume_plan_accepts_offline_stage_selection(tmp_path, stage_args):
    parser = argparse.ArgumentParser()
    activation.add_activation_arguments(parser)
    args = parser.parse_args([
        "--db-path", str(tmp_path / "absent.duckdb"), "--dry-run", "--force",
        "--submissions-resume-from-run-id", "prior-dataset", *stage_args,
    ])
    events = []

    def run(options, *, stages):
        events.extend(activation.run_activation(options, stages=stages, emit=lambda event: None))

    activation.run_activation_from_args(args, governed_migrations=lambda path: pytest.fail("unexpected migration"),
                                        run_activation=run)
    assert events[0]["stage"] == "submissions_load"
    assert all(event["stage"] != "sec_bulk_download" for event in events)
