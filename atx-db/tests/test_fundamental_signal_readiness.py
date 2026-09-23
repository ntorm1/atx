"""Small, isolated fixtures for the optional FQ1/FQ2 readiness inventory."""

from __future__ import annotations

import datetime as dt
import importlib.util
import json
from pathlib import Path

import duckdb
import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "measure_tier1_readiness.py"
SPEC = importlib.util.spec_from_file_location("measure_tier1_readiness", SCRIPT)
assert SPEC and SPEC.loader
readiness = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(readiness)


@pytest.fixture
def con():
    connection = duckdb.connect(":memory:", config={"memory_limit": "256MB", "threads": "1"})
    try:
        yield connection
    finally:
        connection.close()


def tables(con):
    con.execute("""
        CREATE TABLE fundamental_signal_runs (
            run_id VARCHAR, status VARCHAR, as_of_date DATE, run_at TIMESTAMP,
            start_date DATE, end_date DATE, panel_sha256 VARCHAR,
            blockers_json VARCHAR, production_eligible BOOLEAN
        );
        CREATE TABLE fundamental_signal_coverage (
            run_id VARCHAR, signal_id VARCHAR, decision_date DATE, status VARCHAR,
            visible_members BIGINT, common_members BIGINT, overlap_members BIGINT,
            qualified_cik BIGINT, unmatched_owner_states BIGINT,
            eligible_scores BIGINT, excluded_scores BIGINT, constant_scores BOOLEAN
        );
        CREATE TABLE fundamental_signal_evaluation_runs (
            run_id VARCHAR, build_run_id VARCHAR, status VARCHAR, as_of_date DATE,
            run_at TIMESTAMP, build_sha256 VARCHAR, result_sha256 VARCHAR,
            blockers_json VARCHAR, production_eligible BOOLEAN
        );
        CREATE TABLE fundamental_signal_evaluation_summaries (
            run_id VARCHAR, signal_id VARCHAR, horizon_sessions INTEGER, split VARCHAR,
            primary_hypothesis BOOLEAN, status VARCHAR, spread_dates BIGINT,
            eligible_count BIGINT, labeled_count BIGINT, label_coverage DOUBLE,
            holm_p_value DOUBLE, net_25bp DOUBLE, candidate BOOLEAN,
            production_eligible BOOLEAN
        );
    """)


def measure(con):
    return readiness.Measurement(con, dt.date(2026, 9, 20)).fundamental_signals()


def test_absent_and_partial_schemas_are_separate(con):
    result = measure(con)
    assert result["fq1"]["manifest"]["schema_gaps"]["fundamental_signal_runs"] == "absent relation"
    assert result["fq2"]["manifest"]["schema_gaps"]["fundamental_signal_evaluation_runs"] == "absent relation"
    con.execute("CREATE TABLE fundamental_signal_runs (run_id VARCHAR)")
    result = measure(con)
    assert "status" in result["fq1"]["manifest"]["schema_gaps"]["fundamental_signal_runs"]["missing_columns"]
    assert result["fq2"]["status"] == "unmeasured"


def test_selected_snapshot_and_hash_mismatch_suppress_candidate(con):
    tables(con)
    con.execute("""
        INSERT INTO fundamental_signal_runs VALUES
          ('old','complete','2026-09-19','2026-09-19 22:00:00','2026-01-01','2026-09-19','abc','[]',false),
          ('future','complete','2026-09-21','2026-09-21 22:00:00','2026-01-01','2026-09-21','future','[]',false),
          ('unfinished','building','2026-09-20','2026-09-20 22:00:00','2026-01-01','2026-09-20',NULL,'["private@example.com"]',false);
        INSERT INTO fundamental_signal_coverage VALUES
          ('old','quality','2026-09-18','ok',100,90,80,70,2,60,20,false),
          ('future','quality','2026-09-21','ok',100,90,80,70,2,60,20,false);
        INSERT INTO fundamental_signal_evaluation_runs VALUES
          ('eval_old','old','complete','2026-09-19','2026-09-19 23:00:00','abc','xyz','[]',false),
          ('eval_bad','old','complete','2026-09-20','2026-09-20 23:00:00','wrong','xyz','["private@example.com"]',false);
        INSERT INTO fundamental_signal_evaluation_summaries VALUES
          ('eval_old','quality',21,'test',true,'ok',5,60,50,0.83,0.1,0.02,true,false),
          ('eval_bad','quality',21,'test',true,'ok',5,60,50,0.83,0.1,0.02,true,false);
    """)
    result = measure(con)
    assert result["fq1"]["selected_run"]["run_id"] == "old"
    assert result["fq1"]["coverage"]["rows"][0]["sessions"] == 1
    assert result["fq2"]["selected_run"]["run_id"] == "eval_bad"
    assert result["fq2"]["selected_run"]["recorded_hash_equality"] is False
    assert result["fq2"]["summaries"] is None or result["fq2"]["summaries"]["rows"] == []
    rendered = json.dumps(readiness.safe(result), default=str)
    assert "private@example.com" not in rendered
    assert "recorded_research_candidate" not in rendered
    assert result["certification"] == "unmeasured"


def test_bounded_inventories_and_frozen_summaries(con, monkeypatch):
    tables(con)
    con.execute("INSERT INTO fundamental_signal_runs VALUES ('build','complete','2026-09-20','2026-09-20 22:00:00','2026-01-01','2026-09-20','hash','[]',false)")
    con.execute("INSERT INTO fundamental_signal_evaluation_runs VALUES ('eval','build','complete','2026-09-20','2026-09-20 23:00:00','hash','result','[]',false)")
    con.execute("INSERT INTO fundamental_signal_evaluation_summaries VALUES ('eval','quality',21,'holdout',true,'candidate',5,60,50,0.83,0.1,0.02,true,false)")
    for index in range(4):
        con.execute("INSERT INTO fundamental_signal_coverage VALUES ('build',?, '2026-09-20','ok',100,90,80,70,2,60,20,false)", [f"signal_{index}"])
    for index in range(3):
        con.execute("INSERT INTO fundamental_signal_runs VALUES (?, 'blocked_empty','2026-09-20','2026-09-20 23:00:00','2026-01-01','2026-09-20',NULL,'[]',false)", [f"blocked_{index}"])
    measurement = readiness.Measurement(con, dt.date(2026, 9, 20))
    for table in ("fundamental_signal_runs", "fundamental_signal_coverage",
                  "fundamental_signal_evaluation_runs", "fundamental_signal_evaluation_summaries"):
        measurement.columns(table)
    monkeypatch.setattr(readiness, "ROW_LIMIT", 2)
    result = measurement.fundamental_signals()
    assert result["fq1"]["manifest"]["truncated"] is True
    assert result["fq1"]["selected_run"]["run_id"] == "build"
    assert result["fq1"]["coverage"]["truncated"] is True
    assert len(result["fq1"]["coverage"]["rows"]) == 2
    assert result["fq2"]["summaries"]["rows"][0]["recorded_research_candidate"] is True
    assert result["fq2"]["selected_run"]["recorded_hash_equality"] is True
    report = {"as_of_date": "2026-09-20", "generated_at_utc": "now", "surfaces": {},
              "activation": {}, "annual_item_coverage": {}, "provider_coverage": {},
              "historical_gaps": {}, "cf1": {}, "fundamental_signal_research": result,
              "quality": {}, "limitations": []}
    md = readiness.markdown(readiness.safe(report))
    assert "Recorded FQ1/FQ2 research evidence" in md
    assert '"certification": "unmeasured"' in md


def test_evaluation_cannot_use_build_from_later_snapshot(con):
    tables(con)
    con.execute("INSERT INTO fundamental_signal_runs VALUES ('later_build','complete','2026-09-20','2026-09-20 22:00:00','2026-01-01','2026-09-20','hash','[]',false)")
    con.execute("INSERT INTO fundamental_signal_evaluation_runs VALUES ('earlier_eval','later_build','complete','2026-09-19','2026-09-20 23:00:00','hash','result','[]',false)")
    con.execute("INSERT INTO fundamental_signal_evaluation_summaries VALUES ('earlier_eval','quality',21,'holdout',true,'candidate',5,60,50,0.83,0.1,0.02,true,false)")
    result = measure(con)
    assert result["fq2"]["selected_run"]["run_id"] == "earlier_eval"
    assert result["fq2"]["selected_run"]["recorded_hash_equality"] is True
    assert result["fq2"]["selected_run"]["linked_build_snapshot_eligible"] is False
    assert result["fq2"]["summaries"] is None


def test_fq2_selection_survives_missing_fq1_schema(con):
    tables(con)
    con.execute("DROP TABLE fundamental_signal_runs")
    con.execute("INSERT INTO fundamental_signal_evaluation_runs VALUES ('eval','build','complete','2026-09-20','2026-09-20 23:00:00','hash','result','[]',false)")
    result = measure(con)
    assert result["fq2"]["selected_run"]["run_id"] == "eval"
    assert result["fq2"]["selected_run"]["linkage_status"] == "unmeasured"
    assert result["fq2"]["selected_run"]["linkage_schema_gaps"]["fundamental_signal_runs"] == "absent relation"
    assert result["fq2"]["summaries"] is None
