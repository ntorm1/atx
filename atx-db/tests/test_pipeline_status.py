"""Small ledger-only fixtures for the read-only pipeline report."""

from __future__ import annotations

import datetime as dt
import json
import uuid

import duckdb

from atx_db import cli
from atx_db.activation import STAGE_ORDER
from atx_db.migrations import MIGRATIONS
from atx_db.pipeline_status import pipeline_status

DAY = dt.date(2026, 9, 22)
START = dt.datetime(2026, 9, 22, 10)
END = dt.datetime(2026, 9, 22, 11)


def _db(tmp_path):
    path = tmp_path / "ledger.duckdb"
    con = duckdb.connect(str(path))
    con.execute("CREATE TABLE schema_migrations (version VARCHAR)")
    con.execute("""CREATE TABLE activation_stage_runs (
        stage VARCHAR, run_id VARCHAR, status VARCHAR, started_at TIMESTAMP,
        finished_at TIMESTAMP, "rows" BIGINT, params_json VARCHAR, error VARCHAR
    )""")
    con.execute("""CREATE TABLE dataset_runs (
        run_id VARCHAR, dataset_id VARCHAR, status VARCHAR, started_at TIMESTAMP,
        finished_at TIMESTAMP, rows_loaded BIGINT, source VARCHAR,
        params_json VARCHAR, error_message VARCHAR
    )""")
    con.executemany("INSERT INTO schema_migrations VALUES (?)", [(str(m.version),) for m in MIGRATIONS])
    return path, con


def _stage(con, stage, *, run_id="activation-1", status="failed", day=DAY,
           started=START, finished=END, params=None, error="secret traceback"):
    if params is None:
        params = {"as_of_date": day.isoformat(), "sec_user_agent": "private@example.com"}
    con.execute(
        'INSERT INTO activation_stage_runs VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
        [stage, run_id, status, started, finished, 42, json.dumps(params), error],
    )


def _source(con, stage, label, *, run_id=None, status="failed", started=None,
            params=None, error="secret source failure"):
    dataset_id = {
        "companyfacts_load": "sec_company_facts",
        "submissions_load": "sec_submissions",
        "earnings_release_facts": "sec_earnings_release_facts",
    }[stage]
    run_id = run_id or str(uuid.uuid4())
    con.execute(
        "INSERT INTO dataset_runs VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        [run_id, dataset_id, status, started or (START + dt.timedelta(minutes=1)),
         END - dt.timedelta(minutes=1), 12, "sec", json.dumps(params or {"run_id": label,
         "user_agent": "private@example.com"}), error],
    )
    return run_id


def _item(report, name):
    return next(item for item in report["stages"] if item["stage"] == name)


def test_schema_current_but_snapshot_incomplete(tmp_path):
    path, con = _db(tmp_path)
    _stage(con, "migrate", status="completed", error=None)
    con.close()
    report = pipeline_status(path, DAY)
    assert report["status"] == "ok"
    assert report["migrations"]["schema_current"] is True
    assert report["ledger_completion"] == "incomplete"
    assert report["production_readiness"] == "unassessed"
    assert [item["stage"] for item in report["stages"]] == list(STAGE_ORDER)
    assert _item(report, "companyfacts_load")["status"] == "missing"


def test_failed_source_returns_real_uuid_and_no_secrets(tmp_path):
    path, con = _db(tmp_path)
    _stage(con, "companyfacts_load", params={"as_of_date": DAY.isoformat(),
           "companyfacts_limit": 30, "companyfacts_symbol_source": "archive_members",
           "sec_user_agent": "private@example.com"})
    actual = _source(con, "companyfacts_load", "activation-1-companyfacts")
    con.close()
    report = pipeline_status(path, DAY)
    item = _item(report, "companyfacts_load")
    assert item["source_attempt"]["resume_candidate_run_id"] == actual
    assert item["source_attempt"]["run_id"] == actual
    assert item["scope"] == {"companyfacts_limit": 30,
                              "companyfacts_symbol_source": "archive_members"}
    assert "private@example.com" not in json.dumps(report)
    assert "secret" not in json.dumps(report)


def test_running_is_not_live_process_evidence(tmp_path):
    path, con = _db(tmp_path)
    _stage(con, "submissions_load", status="running", finished=None, error=None)
    con.close()
    report = pipeline_status(path, DAY)
    assert _item(report, "submissions_load")["status"] == "running"
    assert report["process_liveness"] == "unknown"
    assert report["operator_check_needed"] is True
    assert _item(report, "submissions_load")["source_attempt"]["match"] == "unknown"


def test_other_snapshot_and_bad_metadata_are_not_borrowed(tmp_path):
    path, con = _db(tmp_path)
    _stage(con, "migrate", day=dt.date(2026, 9, 21))
    _stage(con, "companyfacts_load", params={"as_of_date": "not-a-date"})
    _stage(con, "periods", params={"run_id": "activation-1"})
    con.close()
    report = pipeline_status(path, DAY)
    assert all(item["status"] == "missing" for item in report["stages"])
    assert report["unattributed_attempts_present"] is True


def test_lower_version_gap_is_reported(tmp_path):
    path, con = _db(tmp_path)
    versions = sorted(m.version for m in MIGRATIONS)
    gap = versions[-2]
    con.execute("DELETE FROM schema_migrations WHERE version = ?", [str(gap)])
    con.close()
    migrations = pipeline_status(path, DAY)["migrations"]
    assert gap in migrations["lower_version_gaps"]
    assert migrations["schema_current"] is False


def test_ambiguous_source_is_never_a_resume_candidate(tmp_path):
    path, con = _db(tmp_path)
    _stage(con, "submissions_load")
    _source(con, "submissions_load", "activation-1-submissions")
    _source(con, "submissions_load", "activation-1-submissions")
    con.close()
    source = _item(pipeline_status(path, DAY), "submissions_load")["source_attempt"]
    assert source["match"] == "ambiguous"
    assert source["resume_candidate_run_id"] is None


def test_missing_database_and_ledger_table(tmp_path):
    missing = tmp_path / "absent.duckdb"
    report = pipeline_status(missing, DAY)
    assert report["reason"] == "database_missing"
    assert not missing.exists()
    path = tmp_path / "partial.duckdb"
    con = duckdb.connect(str(path))
    con.execute("CREATE TABLE schema_migrations (version VARCHAR)")
    con.close()
    report = pipeline_status(path, DAY)
    assert report["reason"] == "ledger_table_missing"
    assert report["missing_tables"] == ["activation_stage_runs", "dataset_runs"]


def test_cli_exit_and_read_only_open_configuration(tmp_path, monkeypatch, capsys):
    path, con = _db(tmp_path)
    con.close()
    original_connect = duckdb.connect
    observed = []

    def checked_connect(*args, **kwargs):
        observed.append(kwargs)
        return original_connect(*args, **kwargs)

    monkeypatch.setattr(duckdb, "connect", checked_connect)
    assert cli.main(["pipeline-status", "--db-path", str(path),
                     "--as-of-date", DAY.isoformat()]) == 0
    assert json.loads(capsys.readouterr().out)["status"] == "ok"
    assert observed == [{"read_only": True, "config": {
        "memory_limit": "256MB", "threads": "1", "TimeZone": "UTC"}}]
    assert cli.main(["pipeline-status", "--db-path", str(tmp_path / "absent.duckdb"),
                     "--as-of-date", DAY.isoformat()]) == 2
