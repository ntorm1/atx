"""Small inspection fixtures for the validated FQ1 desk reader."""

from __future__ import annotations

import datetime as dt
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import duckdb
import pytest

from atx_db.fundamental_signal_research import (
    FundamentalSignalResearchOptions,
    build_fundamental_signal_panel,
    canonical_signals,
    validate_fundamental_signal_panel,
)
from atx_db.migrations.bodies_0324 import create_fundamental_signal_tables

pytest_plugins = ("tests.test_fundamental_signal_research",)


ROOT = Path(__file__).resolve().parents[1]
SQL = (ROOT / "sql/research/fundamental-desk-screen-acceptance.sql").read_text()
script_spec = importlib.util.spec_from_file_location(
    "desk_reader", ROOT / "scripts/read_fundamental_desk_screen.py")
assert script_spec is not None and script_spec.loader is not None
reader = importlib.util.module_from_spec(script_spec)
script_spec.loader.exec_module(reader)


@pytest.fixture(autouse=True)
def bounded_duckdb_connect(monkeypatch):
    real_connect = duckdb.connect

    def connect(*args, **kwargs):
        config = dict(kwargs.pop("config", {}))
        config.update({"memory_limit": "256MB", "threads": "1"})
        return real_connect(*args, config=config, **kwargs)

    monkeypatch.setattr(duckdb, "connect", connect)


def fixture():
    con = duckdb.connect(":memory:", config={"memory_limit": "256MB", "threads": "1"})
    create_fundamental_signal_tables(con)
    con.execute("""
      CREATE TABLE market_daily_metrics (
        market_daily_id VARCHAR,source VARCHAR,security_id VARCHAR,symbol VARCHAR,
        trade_date DATE,as_of_date DATE,available_at TIMESTAMP,run_id VARCHAR,
        inputs_hash VARCHAR,close DOUBLE,market_cap DOUBLE,
        earnings_yield DOUBLE,cfo_to_ev DOUBLE)
    """)
    con.execute("""
      INSERT INTO fundamental_signal_coverage
      (run_id,signal_id,decision_date,decision_at,entry_date,status,
       visible_members,common_members,overlap_members,qualified_cik,
       unmatched_owner_states,eligible_scores,excluded_scores,
       constant_scores,reasons_json)
      SELECT 'build_a',sid,DATE '2026-09-18',TIMESTAMP '2026-09-18 22:00:00',
        DATE '2026-09-21','eligible',1,1,0,1,0,1,0,false,'{}'
      FROM (VALUES ('eps_growth_yoy'),('operating_margin_change_yoy'),
        ('low_total_accruals'),('low_net_debt_ebitda')) t(sid)
    """)
    legs = [
        ("eps_growth_yoy", 0.2, 0.2),
        ("operating_margin_change_yoy", 0.03, 0.03),
        ("low_total_accruals", -0.11, 0.11),
        ("low_net_debt_ebitda", -2.4, 2.4),
    ]
    for sid, score, raw in legs:
        con.execute("""
          INSERT INTO fundamental_signal_values
          (run_id,signal_id,decision_date,security_id,decision_at,entry_date,
           score,eligible,reason,cohort_size)
          VALUES ('build_a',?,DATE '2026-09-18','trading_1',
            TIMESTAMP '2026-09-18 22:00:00',DATE '2026-09-21',?,true,'valid',1)
        """, [sid, score])
        con.execute("""
          INSERT INTO fundamental_signal_inputs
          (run_id,signal_id,decision_date,security_id,term_ordinal,
           metric_code,metric_window,weight,derived_value_id,
           derived_owner_security_id,selected_input_cik,lineage_status,
           fiscal_period_end,available_at,raw_value,reason)
          VALUES ('build_a',?,DATE '2026-09-18','trading_1',0,
            'metric','ttm',1,?,'issuer_1',
            '0000000001','qualified',DATE '2026-06-30',
            TIMESTAMP '2026-08-01 12:00:00',?,'valid')
        """, [sid, sid, raw])
    con.execute("""
      INSERT INTO market_daily_metrics VALUES
      ('older','atx-db daily market panel v1','trading_1','OLD',
       DATE '2026-09-18',DATE '2026-09-18',
       TIMESTAMP '2026-09-18 20:00:00','market_run','hash',10,100,0.04,0.08),
      ('latest','atx-db daily market panel v1','trading_1','NEW',
       DATE '2026-09-18',DATE '2026-09-18',
       TIMESTAMP '2026-09-18 21:00:00','market_run','hash',11,110,NULL,0.09),
      ('late','atx-db daily market panel v1','trading_1','LATE',
       DATE '2026-09-18',DATE '2026-09-18',
       TIMESTAMP '2026-09-18 23:00:00','market_run','hash',12,120,0.05,0.1),
      ('other_session','atx-db daily market panel v1','trading_1','FUTURE',
       DATE '2026-09-21',DATE '2026-09-21',
       TIMESTAMP '2026-09-21 21:00:00','market_run','hash',13,130,0.05,0.1)
    """)
    return con


def inspect(con):
    rows = con.execute(SQL, ["build_a", dt.date(2026, 9, 20)]).fetchall()
    assert len(rows) <= 1000
    return [json.loads(row[1]) for row in rows]


def test_raw_signs_owner_independent_market_join_and_latest_null_state():
    con = fixture()
    diagnostic, name = inspect(con)
    assert diagnostic["decision_date"] == "2026-09-18"
    assert diagnostic["report_as_of"] == "2026-09-20"
    assert diagnostic["invalid_earnings_yield"] == 1
    assert name["disposition"] == "incomplete"
    assert name["market_daily_id"] == "latest"
    assert name["eps_owner_id"] == "issuer_1"
    assert name["security_id"] == "trading_1"
    assert name["total_accruals"] == 0.11
    assert name["net_debt_ebitda"] == 2.4
    con.execute("UPDATE market_daily_metrics SET earnings_yield=0.04 WHERE market_daily_id='latest'")
    diagnostic, name = inspect(con)
    assert diagnostic["passing"] == 1
    assert name["disposition"] == "passing"


def test_rejected_raw_value_is_hidden_even_when_finite():
    con = fixture()
    con.execute("""
      UPDATE fundamental_signal_inputs SET reason='stale_current_anchor'
      WHERE run_id='build_a' AND signal_id='low_total_accruals'
    """)
    _, name = inspect(con)
    assert name["total_accruals"] is None
    assert name["accruals_input_reason"] == "stale_current_anchor"
    assert name["accruals_state_id"] == "low_total_accruals"


def test_empty_cohort_and_run_isolation():
    con = fixture()
    con.execute("DELETE FROM fundamental_signal_values WHERE run_id='build_a'")
    con.execute("DELETE FROM fundamental_signal_inputs WHERE run_id='build_a'")
    con.execute("""
      INSERT INTO fundamental_signal_values
      (run_id,signal_id,decision_date,security_id,decision_at,entry_date,
       score,eligible,reason,cohort_size)
      VALUES ('build_b','eps_growth_yoy',DATE '2026-09-18','foreign',
        TIMESTAMP '2026-09-18 22:00:00',DATE '2026-09-21',1,true,'valid',1)
    """)
    rows = inspect(con)
    assert len(rows) == 1
    assert rows[0]["status"] == "no_cohort_rows"
    assert rows[0]["cohort_rows"] == 0


def test_preview_cap_does_not_cap_diagnostic_counts():
    con = fixture()
    con.execute("""
      INSERT INTO fundamental_signal_values
      SELECT run_id,signal_id,decision_date,'extra_' || cast(n AS VARCHAR),
        decision_at,entry_date,NULL,false,'missing_metric_state',
        input_end,input_available_at,cohort_size
      FROM fundamental_signal_values,range(1001) t(n)
      WHERE run_id='build_a'
    """)
    con.execute("""
      INSERT INTO fundamental_signal_inputs
      SELECT run_id,signal_id,decision_date,'extra_' || cast(n AS VARCHAR),
        term_ordinal,metric_code,metric_window,weight,
        NULL,NULL,definition_hash,inputs_hash,selected_input_refs_hash,
        lineage_digest,NULL,'missing',lineage_leaf_ids_json,
        selected_leaf_oldest_end,selected_leaf_newest_end,period_end,
        fiscal_period_start,fiscal_period_end,available_at,value_origin,
        history_status,value_status,NULL,'missing_metric_state'
      FROM fundamental_signal_inputs,range(1001) t(n)
      WHERE run_id='build_a'
    """)
    rows = inspect(con)
    assert len(rows) == 1000
    assert rows[0]["cohort_rows"] == 1002
    assert rows[0]["unavailable_eps"] == 1001


def test_transfer_byte_preflight_rejects_one_oversized_field(monkeypatch):
    con = fixture()
    monkeypatch.setattr(reader, "MAX_BYTES", 1024)
    con.execute("UPDATE market_daily_metrics SET symbol=repeat('x',1500) WHERE market_daily_id='latest'")
    with pytest.raises(ValueError, match="transfer byte cap"):
        reader._bounded_rows(con, SQL, "build_a", dt.date(2026, 9, 20))


def test_tiny_real_fq1_final_session_read_and_digest_tamper(tiny_panel_store):
    options = FundamentalSignalResearchOptions(
        start_date=dt.date(2024, 1, 2), end_date=dt.date(2024, 1, 4),
        as_of_date=dt.date(2024, 1, 4),
        run_at=dt.datetime(2024, 1, 4, 22, tzinfo=dt.UTC),
        run_id="desk_tiny",
    )
    result = build_fundamental_signal_panel(tiny_panel_store, options)
    assert result.status == "complete"
    con = tiny_panel_store.con
    assert con.execute("""
      SELECT reason FROM fundamental_signal_values
      WHERE run_id='desk_tiny' AND signal_id='eps_growth_yoy'
        AND decision_date=DATE '2024-01-04' AND security_id='b'
    """).fetchone()[0] == "missing_next_session"
    for definition in (
        "market_daily_id VARCHAR", "security_id VARCHAR", "symbol VARCHAR",
        "run_id VARCHAR", "inputs_hash VARCHAR", "market_cap DOUBLE",
        "earnings_yield DOUBLE", "cfo_to_ev DOUBLE",
    ):
        con.execute(f"ALTER TABLE market_daily_metrics ADD COLUMN {definition}")
    con.execute("""
      UPDATE market_daily_metrics SET
        market_daily_id='market_' || cast(trade_date AS VARCHAR),
        security_id='b',symbol='B',run_id='market_run',inputs_hash='hash',
        market_cap=100,earnings_yield=0.05,cfo_to_ev=0.06
    """)
    report = reader.read_screen(
        con, "desk_tiny", dt.date(2024, 1, 4), SQL,
        validate_fundamental_signal_panel, canonical_signals(None))
    assert report["decision_date"] == "2024-01-04"
    assert report["diagnostic"]["four_score_eligible"] == 0
    selected = next(row for row in report["preview"] if row["security_id"] == "b")
    assert selected["eps_reason"] == "missing_next_session"
    assert selected["eps_qualified"] is True
    assert selected["disposition"] == "passing"
    con.execute("""
      UPDATE fundamental_signal_inputs SET raw_value=raw_value+1
      WHERE run_id='desk_tiny' AND signal_id='eps_growth_yoy'
        AND decision_date=DATE '2024-01-04' AND security_id='b'
    """)
    with pytest.raises(ValueError, match="digest mismatch"):
        reader.read_screen(con, "desk_tiny", dt.date(2024, 1, 4), SQL,
                           validate_fundamental_signal_panel, canonical_signals(None))


def test_fresh_output_path_and_overwrite_refusal(tmp_path):
    db_path = tmp_path / "warehouse.duckdb"
    con = duckdb.connect(str(db_path), config={"memory_limit": "256MB", "threads": "1"})
    create_fundamental_signal_tables(con)
    con.close()
    output = tmp_path / "desk.json"
    args = ["--db-path", str(db_path), "--build-run-id", "missing",
            "--report-as-of", "2026-09-20", "--output-json", str(output)]
    assert reader.main(args) == 2
    assert not output.exists()
    output.write_text("existing", encoding="utf-8")
    with pytest.raises(SystemExit):
        reader.main(args)
    assert output.read_text(encoding="utf-8") == "existing"


def test_real_validator_rejects_absent_completed_panel_and_spec_mismatch():
    con = fixture()
    with pytest.raises(ValueError, match="absent or not complete"):
        reader.read_screen(con, "build_a", dt.date(2026, 9, 20), SQL,
                           validate_fundamental_signal_panel, canonical_signals(None))
    specs = canonical_signals(None)
    fake_verified = SimpleNamespace(
        run_id="build_a", specs=specs[:-1], panel_sha256="hash",
        decision_dates=(dt.date(2026, 9, 18),), calendar_sha256="hash")
    with pytest.raises(ValueError, match="frozen default"):
        reader.read_screen(con, "build_a", dt.date(2026, 9, 20), SQL,
                           lambda *_: fake_verified, specs)
