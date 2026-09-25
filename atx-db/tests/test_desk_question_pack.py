"""Read-only operation and economic refusal checks using existing tiny fixtures."""

from __future__ import annotations

import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path

import duckdb
import pytest

from tests.test_market_daily import _fact

pytest_plugins = ("tests.test_fundamental_signal_research",)
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("desk_question_pack", ROOT / "scripts/read_desk_question_pack.py")
assert spec is not None and spec.loader is not None
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


@pytest.fixture(autouse=True)
def bounded_connections(monkeypatch):
    real_connect = duckdb.connect

    def connect(*args, **kwargs):
        config = dict(kwargs.pop("config", {}))
        config.update(memory_limit="256MB", threads="1")
        return real_connect(*args, config=config, **kwargs)

    monkeypatch.setattr(duckdb, "connect", connect)


def query(con, key, **params):
    sql = (ROOT / "sql/research" / reader.SQL_FILES[key]).read_text(encoding="utf-8")
    cursor = con.execute(sql, params)
    columns = [column[0] for column in cursor.description]
    return [dict(zip(columns, row, strict=True)) for row in cursor.fetchall()]


def test_real_read_only_connection_rejects_mutation_and_keeps_bytes(tmp_path):
    path = tmp_path / "small.duckdb"
    with duckdb.connect(str(path)) as con:
        con.execute("CREATE TABLE protected(value INTEGER)")
    before = hashlib.sha256(path.read_bytes()).hexdigest()
    with reader.open_read_only(path, "256MB", 1) as con:
        with pytest.raises(duckdb.Error, match="read-only"):
            con.execute("INSERT INTO protected VALUES (1)")
        assert con.execute("SELECT count(*) FROM protected").fetchone()[0] == 0
    assert hashlib.sha256(path.read_bytes()).hexdigest() == before


@pytest.mark.parametrize("protected", ["data/warehouse.duckdb", "warehouse_template.duckdb"])
def test_original_database_paths_are_refused_before_connect(protected, tmp_path, monkeypatch):
    def forbidden(*_args, **_kwargs):
        pytest.fail("protected database opened")

    monkeypatch.setattr(reader, "open_read_only", forbidden)
    output = tmp_path / "out.json"
    assert reader.main(["--db-path", str(ROOT / protected), "--output-json", str(output)]) == 2
    assert not output.exists()


def test_runner_caps_rows_and_marks_truncation_on_existing_schema_fixture(built_warehouse, tmp_path):
    path = built_warehouse("desk.duckdb")
    output = tmp_path / "receipt.json"
    before = hashlib.sha256(path.read_bytes()).hexdigest()
    assert reader.main(["--db-path", str(path), "--output-json", str(output),
                        "--query", "q5", "--mode", "run", "--max-rows", "2"]) == 0
    receipt = json.loads(output.read_text())
    result = receipt["queries"][0]
    assert receipt["read_only"] and not receipt["production_qualified"]
    assert result["truncated"] and result["returned_rows"] == 2
    assert {row["status"] for row in result["rows"]} == {"build_manifest_missing"}
    assert hashlib.sha256(path.read_bytes()).hexdigest() == before
    original = output.read_bytes()
    assert reader.main(["--db-path", str(path), "--output-json", str(output)]) == 2
    assert output.read_bytes() == original


def test_connection_failure_writes_an_unavailable_receipt(tmp_path):
    database = tmp_path / "invalid.duckdb"
    database.write_bytes(b"not a database")
    output = tmp_path / "failure.json"
    assert reader.main(["--db-path", str(database), "--output-json", str(output)]) == 2
    receipt = json.loads(output.read_text())
    assert receipt["status"] == "unavailable" and receipt["queries"] == []
    assert receipt["error_type"] and not receipt["production_qualified"]
    assert database.read_bytes() == b"not a database"


def test_q1_negative_base_restatement_cutoff_and_missing_ttm(tmp_store):
    con = tmp_store.con
    for end, start, value in (
        ("2025-06-30", "2025-04-01", -1.0),
        ("2026-03-31", "2026-01-01", 0.5),
        ("2026-06-30", "2026-04-01", 2.0),
    ):
        day = dt.date.fromisoformat(end)
        at = dt.datetime.combine(day + dt.timedelta(days=35), dt.time(12))
        _fact(tmp_store, "issuer", "eps_diluted", "quarterly", day, value, at)
        con.execute("""
          UPDATE fundamental_standardized SET cik='0000093410',period_start=?,
            fiscal_year=?,fiscal_period=? WHERE period_end=?
        """, [start, day.year, "Q" + str((day.month + 2) // 3), day])
    # SEC companyfacts fy/fp belong to the FILING: the 2026 Q2 10-Q re-reports the
    # prior-year quarter under fy=2026. That later label must not break the YoY.
    con.execute("""
      INSERT INTO fundamental_standardized
      SELECT * REPLACE ('comparative' AS standardized_id,2026 AS fiscal_year,
        TIMESTAMP '2026-08-05 22:00:00' AS available_at,DATE '2026-08-05' AS as_of_date)
      FROM fundamental_standardized WHERE period_end=DATE '2025-06-30'
    """)
    params = {"cik": "93410", "cutoff": dt.datetime(2026, 9, 20, 22)}
    rows = query(con, "q1", **params)
    latest = rows[0]
    assert latest["yoy_growth"] == 3 and latest["qoq_growth"] == 3
    assert latest["yoy_status"] == "negative_base_absolute_denominator"
    assert latest["yoy_label_status"] == "labels_consistent"
    assert latest["ttm_eps"] is None and latest["basic_missing_at_endpoint"]
    prior = next(r for r in rows if r["period_end"] == dt.date(2025, 6, 30))
    assert (prior["fiscal_year"], prior["selected_filing_fiscal_year"]) == (2025, 2026)
    assert prior["visible_revision_count"] == 2
    con.execute("""
      INSERT INTO fundamental_standardized
      SELECT * REPLACE ('later-null' AS standardized_id,NULL AS value,
        TIMESTAMP '2026-09-21 12:00:00' AS available_at,DATE '2026-09-21' AS as_of_date)
      FROM fundamental_standardized WHERE period_end=DATE '2026-06-30'
    """)
    assert query(con, "q1", **params)[0]["visible_revision_count"] == 1
    after = query(con, "q1", **{**params, "cutoff": dt.datetime(2026, 9, 22, 22)})[0]
    assert after["reported_quarter_eps"] is None and after["yoy_growth"] is None
    assert after["visible_revision_count"] == 2 and after["changed_since_first_visible"]


def test_q2_q4_preserve_null_states_missing_metrics_and_raw_signs(tiny_panel_store):
    con = tiny_panel_store.con
    rows = query(con, "q2", cutoff=dt.datetime(2024, 1, 3, 22))
    assert len(rows) == 9
    eps = next(r for r in rows if r["issuer_owner_id"] == "a" and r["metric_code"] == "eps_diluted_q_growth_yoy")
    assert eps["current_state_id"] == "a_eps_null" and eps["latest_yoy"] is None
    assert all(r["acceleration"] is None for r in rows)
    assert sum(r["status"] == "metric_definition_missing" for r in rows) == 6
    # c's newest quarter ended 2023-05-31: never presented as current acceleration.
    stale = next(r for r in rows if r["issuer_owner_id"] == "c" and r["metric_code"] == "eps_diluted_q_growth_yoy")
    assert stale["status"] == "stale_latest_quarter" and stale["acceleration"] is None
    quality = query(con, "q4", cutoff=dt.datetime(2024, 1, 3, 22))
    row = next(r for r in quality if r["issuer_owner_id"] == "a")
    assert row["sloan_accruals"] == -0.1 and row["net_debt_ebitda"] == -0.1
    assert row["screen_status"] == "incomplete" and not row["production_qualified"]


def test_q2_acceleration_requires_an_adjacent_fiscal_span(tiny_panel_store):
    con = tiny_panel_store.con
    con.execute("""
      UPDATE derived_metric_values SET fiscal_period_start='2023-10-01'
      WHERE derived_value_id='b_eps_diluted_q_growth_yoy'
    """)
    con.execute("""
      INSERT INTO derived_metric_values
      SELECT * REPLACE ('prior-quarter' AS derived_value_id,target_bucket-1 AS target_bucket,
        DATE '2023-09-30' AS period_end,DATE '2023-07-01' AS fiscal_period_start,
        DATE '2023-09-30' AS fiscal_period_end,.1 AS value)
      FROM derived_metric_values WHERE derived_value_id='b_eps_diluted_q_growth_yoy'
    """)

    def row():
        return next(r for r in query(con, "q2", cutoff=dt.datetime(2024, 1, 3, 22))
                    if r["issuer_owner_id"] == "b" and r["metric_code"] == "eps_diluted_q_growth_yoy")

    assert row()["acceleration"] == pytest.approx(.1)
    con.execute("""
      UPDATE derived_metric_values SET fiscal_period_start='2023-10-02'
      WHERE derived_value_id='b_eps_diluted_q_growth_yoy'
    """)
    assert row()["status"] == "fiscal_year_change_gap_or_stub" and row()["acceleration"] is None


def test_all_queries_bind_and_empty_history_retains_every_month(tmp_store):
    parameters = {
        "q1": {"cutoff": dt.datetime(2026, 9, 20, 22), "cik": "0000093410"},
        "q2": {"cutoff": dt.datetime(2026, 9, 20, 22)},
        "q3": {"cutoff": dt.datetime(2026, 9, 20, 22), "market_cap_floor": 1e9},
        "q4": {"cutoff": dt.datetime(2026, 9, 20, 22)},
        "q5": {"cutoff": dt.datetime(2026, 9, 20, 22), "build_run_id": "none",
               "evaluation_run_id": "none", "signal_id": "eps_growth_yoy",
               "start_date": dt.date(2015, 1, 1), "end_date": dt.date(2026, 12, 31)},
    }
    for key in ("q1", "q2", "q3", "q4"):
        assert query(tmp_store.con, key, **parameters[key]) == []
    rows = query(tmp_store.con, "q5", **parameters["q5"])
    assert len(rows) == 144 * 2
    assert sum(r["status"] == "future_month" for r in rows) == 6
    assert sum(r["status"] == "month_not_closed_at_cutoff" for r in rows) == 2
    assert all(r["complete_cohort_q10_minus_q1"] is None for r in rows)


def test_q3_trading_owner_bridge_deciles_floor_and_latest_null(tmp_store):
    con = tmp_store.con
    for index in range(12):
        security, owner, cik = f"trading_{index:02}", f"issuer_{index:02}", str(index + 1).zfill(10)
        con.execute("""
          INSERT INTO universe_us_listed_membership
          (membership_id,universe_id,security_id,valid_from,available_at,security_type,
           exchange_code,has_cik,cik,reason,rules_json,decision_count,as_of_date,source)
          VALUES (?,'us_listed_v1',?,'2020-01-01','2020-01-01','common',
            'XNYS',true,?,'fixture','{}',1,'2020-01-01','atx-db us-listed universe builder')
        """, [security, security, cik])
        con.execute("""
          INSERT INTO security_identifier_history
          (security_id,id_type,id_value,valid_from,as_of_date,available_at,source)
          VALUES (?,'CIK',?,'2020-01-01','2020-01-01','2020-01-01','fixture')
        """, [security, cik])
        con.execute("""
          INSERT INTO fundamental_fact_revisions
          (fact_revision_id,revision_group_id,source,security_id,cik,taxonomy,concept,unit,
           period_end,accession_number,filed_date,revision_sequence,revision_count,
           is_latest_revision,is_value_changed,source_url,as_of_date,available_at)
          VALUES (?,?,'fixture',?,?,'us-gaap','NetIncomeLoss','USD','2026-06-30','a',
            '2026-08-01',1,1,true,false,'fixture','2026-08-01','2026-08-01')
        """, [owner, owner, owner, cik])
        con.execute("""
          INSERT INTO market_daily_metrics
          (market_daily_id,source,security_id,trade_date,available_at,inputs_hash,as_of_date,
           market_cap,pe_ttm,ev_ebitda,pb,fcf_yield,ebit_to_ev,enterprise_value)
          VALUES (?,'atx-db daily market panel v1',?,'2026-09-18','2026-09-18 22:00',
            'hash','2026-09-18',?,?,8,2,.05,.1,3000000000)
        """, [security, security, 1e8 if index == 11 else 2e9, float(10 + index)])
        for code in ("roic", "roe", "gross_profitability"):
            con.execute("""
              INSERT INTO derived_metric_values
              (derived_value_id,source,security_id,metric_code,metric_window,period_end,
               available_at,inputs_hash,as_of_date,value,value_status,target_bucket,fiscal_period_end)
              VALUES (?,'atx-db declarative derived metrics v1',?,?,'ttm','2026-06-30',
                '2026-08-01','hash','2026-08-01',.1,'valid',8106,'2026-06-30')
            """, [owner + code, owner, code])
    con.execute("""
      INSERT INTO derived_metric_values
      SELECT * REPLACE ('latest-null-roe' AS derived_value_id,NULL AS value,
        'missing_input_or_domain' AS value_status,TIMESTAMP '2026-08-20' AS available_at,
        DATE '2026-08-20' AS as_of_date)
      FROM derived_metric_values WHERE security_id='issuer_10' AND metric_code='roe'
    """)
    rows = query(con, "q3", cutoff=dt.datetime(2026, 9, 20, 22), market_cap_floor=1e9)
    assert len(rows) == 12
    assert rows[0]["issuer_owner_id"] == "issuer_00" and rows[0]["security_id"] == "trading_00"
    assert [row["pe_decile"] for row in rows[:10]] == list(range(1, 11))
    assert all(row["ranked_names"] == 10 for row in rows[:10])
    assert rows[10]["roe"] is None and rows[10]["status"] == "profitability_missing"
    assert rows[11]["status"] == "market_cap_floor_or_missing"
    assert all(row["pe_decile"] is None for row in rows[10:])


def test_q5_never_substitutes_a_populated_midmonth_for_missing_month_end(tmp_path):
    from atx_db.fundamental_signal_evaluation import (
        FundamentalSignalEvaluationOptions,
        evaluate_fundamental_signals,
    )
    from tests.test_fundamental_signal_evaluation import _warehouse

    store, day, as_of, signal = _warehouse(tmp_path)
    try:
        evaluate_fundamental_signals(store, FundamentalSignalEvaluationOptions(
            build_run_id="build", run_id="eval", as_of_date=as_of,
            run_at=dt.datetime.combine(as_of, dt.time(22, 30), dt.UTC), label_source="labels",
        ))
        rows = query(store.con, "q5", cutoff=dt.datetime.combine(as_of, dt.time(23)),
                     start_date=dt.date(2024, 3, 1), end_date=dt.date(2024, 3, 31),
                     build_run_id="build", evaluation_run_id="eval", signal_id=signal)
        assert len(rows) == 2 and all(r["decision_date"] != day for r in rows)
        assert all(r["status"] == "month_end_not_evaluated" for r in rows)
        assert all(r["complete_cohort_q10_minus_q1"] is None for r in rows)
        assert {r["label_version"] for r in rows} == {"forward_return_publication_v2"}

        def q5_statuses_with_label_version(version):
            store.con.execute(
                "UPDATE fundamental_signal_evaluation_runs "
                "SET config_json=json_merge_patch(config_json, json_object('label_version', ?)) WHERE run_id='eval'",
                [version])
            return {r["status"] for r in query(
                store.con, "q5", cutoff=dt.datetime.combine(as_of, dt.time(23)),
                start_date=dt.date(2024, 3, 1), end_date=dt.date(2024, 3, 31),
                build_run_id="build", evaluation_run_id="eval", signal_id=signal)}

        # Sealed v1 runs stay readable; any other label contract is refused.
        assert q5_statuses_with_label_version("forward_return_publication_v1") == {"month_end_not_evaluated"}
        assert q5_statuses_with_label_version("forward_return_publication_v3") == {"unsupported_evaluation_contract"}
        assert q5_statuses_with_label_version("forward_return_publication_v2") == {"month_end_not_evaluated"}
        assert store.con.execute("""
          SELECT sum(eligible_count),sum(labeled_count) FROM fundamental_signal_evaluation_deciles
          WHERE run_id='eval' AND horizon_sessions=21
        """).fetchone() == (200, 199)
        # Prices vanish after the evaluated mid-March session: that session must not
        # become March's "month end" just because it is the last observed one.
        store.con.execute("DELETE FROM market_daily_metrics WHERE trade_date>? AND trade_date<'2024-04-01'", [day])
        rows = query(store.con, "q5", cutoff=dt.datetime.combine(as_of, dt.time(23)),
                     start_date=dt.date(2024, 3, 1), end_date=dt.date(2024, 3, 31),
                     build_run_id="build", evaluation_run_id="eval", signal_id=signal)
        assert all(r["decision_date"] == day for r in rows)
        assert all(r["status"] == "observed_month_end_session_missing" for r in rows)
        assert all(r["complete_cohort_q10_minus_q1"] is None for r in rows)
    finally:
        store.connection.close()
