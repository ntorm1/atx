"""Focused FQ2 warehouse evaluation without full migration bootstrap."""

from __future__ import annotations

import datetime as dt
import json
from pathlib import Path

import duckdb
import numpy as np
import pytest

from atx_db import _forward_return_publication as publication
from atx_db import fundamental_signal_evaluation as evaluation
from atx_db.connection import DuckDBStore
from atx_db.fundamental_signal_evaluation import (
    FundamentalSignalEvaluationOptions,
    _date_status,
    evaluate_fundamental_signals,
    holm_family,
)
from atx_db.fundamental_signal_research import (
    _BLOCKERS,
    QUERY_VERSION,
    SOURCE_IDS,
    _canonical,
    _panel_digest,
    _sha,
    canonical_signals,
    default_signals,
    validate_fundamental_signal_panel,
)
from atx_db.migrations.bodies_0324 import create_fundamental_signal_tables
from atx_db.migrations.bodies_0325 import create_fundamental_signal_evaluation_tables
from atx_db.research.stats import mean_inference


def test_holm_family_keeps_missing_hypotheses_in_denominator():
    assert holm_family({"a": .01, "b": None, "c": .04}) == {
        "a": .03, "c": .08, "b": None,
    }


def test_canonical_publisher_persists_actual_basis_and_atomic_source_swap(tmp_path, monkeypatch):
    store = DuckDBStore(tmp_path / "publisher.duckdb")
    store.connection = duckdb.connect(str(store.path), config={"threads": 1, "memory_limit": "256MB"})
    con = store.con
    try:
        con.execute("""
            CREATE TABLE equity_daily_bars (
                source VARCHAR,security_id VARCHAR,symbol VARCHAR,trade_date DATE,
                close DOUBLE,adjusted_close DOUBLE,available_at TIMESTAMP,
                vendor_security_id VARCHAR,source_loaded_at TIMESTAMP
            );
            CREATE TABLE trading_calendar (
                calendar_id VARCHAR,trade_date DATE,is_open BOOLEAN,source VARCHAR
            );
            CREATE TABLE delisting_terminal_returns (
                terminal_return_id VARCHAR,security_id VARCHAR,delist_date DATE,
                terminal_return DOUBLE,terminal_return_source VARCHAR,
                return_observation_id VARCHAR,available_at TIMESTAMP,source_loaded_at TIMESTAMP
            );
            CREATE TABLE forward_returns_survivorship_safe (
                forward_return_id VARCHAR PRIMARY KEY,source VARCHAR NOT NULL,
                security_id VARCHAR NOT NULL,symbol VARCHAR,as_of_date DATE NOT NULL,
                horizon_days INTEGER NOT NULL,forward_end_date DATE,
                raw_forward_return DOUBLE,terminal_return DOUBLE,forward_return DOUBLE NOT NULL,
                is_delisted_in_horizon BOOLEAN NOT NULL,is_stitched BOOLEAN NOT NULL,
                delist_date DATE,terminal_return_source VARCHAR,return_observation_id VARCHAR,
                is_latest_revision BOOLEAN NOT NULL DEFAULT true,available_at TIMESTAMP NOT NULL,
                run_id VARCHAR,source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
                updated_at TIMESTAMP NOT NULL DEFAULT now()
            );
        """)
        create_fundamental_signal_evaluation_tables(con)
        days = (dt.date(2024, 1, 2), dt.date(2024, 1, 3))
        con.executemany("""
            INSERT INTO equity_daily_bars VALUES ('prices','S','S',?,?,?,?,?,?)
        """, [(day, close, adjusted, dt.datetime.combine(day, dt.time(22)),
                'vendor', dt.datetime.combine(day, dt.time(22)))
               for day, close, adjusted in zip(days, (100.0, 120.0), (100.0, 110.0), strict=True)])
        con.executemany("""
            INSERT INTO trading_calendar VALUES ('XNYS',?,true,'bars calendar')
        """, [(day,) for day in days])
        con.execute("""
            INSERT INTO forward_returns_survivorship_safe
            (forward_return_id,source,security_id,as_of_date,horizon_days,
             forward_return,is_delisted_in_horizon,is_stitched,available_at)
            VALUES ('foreign','other','X','2024-01-02',1,.5,false,false,'2024-01-03')
        """)
        columns = (
            "forward_return_id", "source", "security_id", "symbol", "as_of_date",
            "horizon_days", "forward_end_date", "raw_forward_return", "terminal_return",
            "forward_return", "is_delisted_in_horizon", "is_stitched", "delist_date",
            "terminal_return_source", "return_observation_id", "is_latest_revision",
            "available_at", "run_id",
        )

        def publish(basis):
            return publication.refresh_forward_return_publication(
                store, source="target", run_id="publish", price_basis=basis,
                cutoff=None, columns=columns, horizons=(1,),
                calendar_id="XNYS", calendar_source="bars calendar",
            )

        assert publish("adjusted_close") == 1
        adjusted = con.execute("""
            SELECT forward_return,price_basis,calculation_version
            FROM forward_returns_survivorship_safe WHERE source='target'
        """).fetchone()
        assert adjusted[0] == pytest.approx(.1)
        assert adjusted[1:] == ("adjusted_close", "forward_return_publication_v2")
        assert con.execute("""
            SELECT price_basis,calculation_version FROM forward_returns_survivorship_safe
            WHERE source='other'
        """).fetchone() == (None, None)
        assert publish("close") == 1
        before = con.execute("""
            SELECT source,forward_return,price_basis,calculation_version
            FROM forward_returns_survivorship_safe ORDER BY source
        """).fetchall()
        assert before[0] == ("other", .5, None, None)
        assert before[1][1] == pytest.approx(.2)
        assert (before[1][0], *before[1][2:]) == (
            "target", "close", "forward_return_publication_v2")

        def fail_swap(*_args, **_kwargs):
            raise RuntimeError("synthetic swap failure")

        monkeypatch.setattr(publication, "publish_validated_shadow", fail_swap)
        with pytest.raises(RuntimeError, match="synthetic swap failure"):
            publish("adjusted_close")
        assert con.execute("""
            SELECT source,forward_return,price_basis,calculation_version
            FROM forward_returns_survivorship_safe ORDER BY source
        """).fetchall() == before
    finally:
        store.connection.close()


def test_observed_session_embargo_and_split_crossing():
    con = duckdb.connect(":memory:")
    try:
        con.execute("CREATE TEMP TABLE _fq2_calendar (trade_date DATE,session_number BIGINT)")
        dates = [dt.date(2023, 12, 20) + dt.timedelta(days=index) for index in range(100)]
        con.executemany("INSERT INTO _fq2_calendar VALUES (?,?)",
                        [(day, index + 1) for index, day in enumerate(dates)])
        cutoff = dt.datetime(2024, 4, 1, 22)
        assert _date_status(con, dates[8], dates[9], 21, 200, False, cutoff)[2] == "purged_split_crossing"
        assert _date_status(con, dates[20], dates[21], 5, 200, False, cutoff)[2] == "embargo"
        assert _date_status(con, dates[80], dates[81], 5, 200, False, cutoff)[2] == "evaluated"
    finally:
        con.close()


def _warehouse(tmp_path):
    store = DuckDBStore(tmp_path / "fq2.duckdb")
    store.connection = duckdb.connect(str(store.path), config={"threads": 1, "memory_limit": "256MB"})
    con = store.con
    con.execute("""
        CREATE TABLE market_daily_metrics (
            source VARCHAR,trade_date DATE,as_of_date DATE,available_at TIMESTAMP,
            close DOUBLE
        );
        CREATE TABLE forward_returns_survivorship_safe (
            forward_return_id VARCHAR PRIMARY KEY,source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,symbol VARCHAR,as_of_date DATE NOT NULL,
            horizon_days INTEGER NOT NULL,forward_end_date DATE,
            raw_forward_return DOUBLE,terminal_return DOUBLE,forward_return DOUBLE NOT NULL,
            is_delisted_in_horizon BOOLEAN NOT NULL,is_stitched BOOLEAN NOT NULL,
            delist_date DATE,terminal_return_source VARCHAR,return_observation_id VARCHAR,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,available_at TIMESTAMP NOT NULL,
            run_id VARCHAR,source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        );
    """)
    create_fundamental_signal_tables(con)
    create_fundamental_signal_evaluation_tables(con)
    create_fundamental_signal_evaluation_tables(con)
    days = [dt.date(2024, 1, 1) + dt.timedelta(days=index) for index in range(140)]
    day, entry = days[70:72]
    as_of = days[-1]
    con.executemany("""
        INSERT INTO market_daily_metrics VALUES (?,?,?,?,100)
    """, [(SOURCE_IDS["market_source"], date, date,
            dt.datetime.combine(date, dt.time(22))) for date in days])
    spec = canonical_signals((default_signals()[0],))[0]
    signal = spec["signal_id"]
    spec_json = _canonical({"signals": (spec,), "max_age_days": 200})
    spec_sha = _sha(spec_json)
    definitions_json = _canonical([{"synthetic_test_definition": True}])
    definitions_sha = _sha(definitions_json)
    calendar_sha = _sha(_canonical([(day.isoformat(), entry.isoformat())]))
    code_sha = "0" * 64
    con.execute("""
        INSERT INTO fundamental_signal_runs
        (run_id,status,spec_json,spec_sha256,definitions_json,definitions_sha256,
         query_version,code_sha256,source_ids_json,calendar_sha256,start_date,end_date,
         as_of_date,run_at,decision_policy,diagnostic_json,blockers_json,panel_sha256)
        VALUES ('build','complete',?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
    """, [spec_json, spec_sha, definitions_json, definitions_sha, QUERY_VERSION,
          code_sha, _canonical(SOURCE_IDS), calendar_sha, day, day, as_of,
          dt.datetime.combine(as_of, dt.time(22)),
          "T 22:00 UTC; next observed session close",
          _canonical({"sessions": 1, "values": 200, "eligible_values": 200,
                      "coverage_rows": 1, "common_membership_rows": 200,
                      "unmatched_owner_states": 0}),
          _canonical(_BLOCKERS), "0"*64])
    con.execute("""
        INSERT INTO fundamental_signal_definitions VALUES ('build',?,0,?,?)
    """, [signal, _canonical(spec), _sha(_canonical(spec))])
    con.execute("""
        INSERT INTO fundamental_signal_coverage
        (run_id,signal_id,decision_date,decision_at,entry_date,status,
         visible_members,common_members,overlap_members,qualified_cik,
         unmatched_owner_states,eligible_scores,excluded_scores,constant_scores,reasons_json)
        VALUES ('build',?,?,? ,?,'complete',200,200,200,200,0,200,0,false,'{}')
    """, [signal, day, dt.datetime.combine(day, dt.time(22)), entry])
    values = []
    inputs = []
    labels = []
    term = spec["terms"][0]
    for index in range(200):
        security = f"S{index:03d}"
        values.append(("build", signal, day, security,
                       dt.datetime.combine(day, dt.time(22)), entry,
                       float(index), True, "valid", 200))
        inputs.append(("build", signal, day, security, 0,
                       term["metric_code"], term["metric_window"], term["weight"], "valid"))
        for horizon in (5, 21, 63):
            if index == 199 and horizon == 21:
                continue
            ending = days[71 + horizon]
            labels.append((f"L{index:03d}_{horizon}", "labels", security,
                           entry, horizon, ending, .001*index, .001*index, False, False,
                           dt.datetime.combine(ending, dt.time(13)),
                           "adjusted_close", "forward_return_publication_v2"))
    con.executemany("""
        INSERT INTO fundamental_signal_values
        (run_id,signal_id,decision_date,security_id,decision_at,entry_date,
         score,eligible,reason,cohort_size) VALUES (?,?,?,?,?,?,?,?,?,?)
    """, values)
    con.executemany("""
        INSERT INTO fundamental_signal_inputs
        (run_id,signal_id,decision_date,security_id,term_ordinal,
         metric_code,metric_window,weight,reason) VALUES (?,?,?,?,?,?,?,?,?)
    """, inputs)
    con.executemany("""
        INSERT INTO forward_returns_survivorship_safe
        (forward_return_id,source,security_id,as_of_date,horizon_days,
         forward_end_date,raw_forward_return,forward_return,
         is_delisted_in_horizon,is_stitched,
         available_at,price_basis,calculation_version)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)
    """, labels)
    panel_sha = _panel_digest(con, "build", spec_sha, definitions_sha, code_sha, calendar_sha)
    con.execute("UPDATE fundamental_signal_runs SET panel_sha256=? WHERE run_id='build'",
                [panel_sha])
    return store, day, as_of, signal


def test_actual_bounded_warehouse_evaluation_and_frozen_membership(tmp_path):
    store, day, as_of, signal = _warehouse(tmp_path)
    try:
        assert validate_fundamental_signal_panel(store.con, "build").eligible_count == 200
        result = evaluate_fundamental_signals(store, FundamentalSignalEvaluationOptions(
            build_run_id="build", run_id="eval", as_of_date=as_of,
            run_at=dt.datetime.combine(as_of, dt.time(22, 30), dt.UTC),
            label_source="labels",
        ))
        assert result["summary_rows"] == 9
        rows = store.con.execute("""
            SELECT decile,eligible_count,labeled_count,missing_count
            FROM fundamental_signal_evaluation_deciles
            WHERE run_id='eval' AND signal_id=? AND decision_date=?
              AND horizon_sessions=21 ORDER BY decile
        """, [signal, day]).fetchall()
        assert len(rows) == 10
        assert rows[0] == (1, 20, 20, 0)
        assert rows[-1] == (10, 20, 19, 1)
        assert store.con.execute("""
            SELECT status,production_eligible,label_rows FROM fundamental_signal_evaluation_runs
            WHERE run_id='eval'
        """).fetchone() == ("complete", False, 599)
        with pytest.raises(ValueError, match="already exists"):
            evaluate_fundamental_signals(store, FundamentalSignalEvaluationOptions(
                build_run_id="build", run_id="eval", as_of_date=as_of,
                run_at=dt.datetime.combine(as_of, dt.time(22, 30), dt.UTC),
                label_source="labels",
            ))
        sealed = store.con.execute("""
            SELECT result_sha256 FROM fundamental_signal_evaluation_runs WHERE run_id='eval'
        """).fetchone()[0]
        assert evaluation._result_digest(store.con, "eval")[1] == sealed
        store.con.execute("""
            UPDATE fundamental_signal_evaluation_deciles
            SET tied_boundary_count=tied_boundary_count+1
            WHERE run_id='eval' AND signal_id=? AND decision_date=?
              AND horizon_sessions=21 AND decile=1
        """, [signal, day])
        assert evaluation._result_digest(store.con, "eval")[1] != sealed
    finally:
        store.connection.close()


def test_tampered_frozen_panel_is_rejected_before_evaluation(tmp_path):
    store, _, _, _ = _warehouse(tmp_path)
    try:
        store.con.execute("UPDATE fundamental_signal_values SET score=999 WHERE security_id='S000'")
        with pytest.raises(ValueError, match="digest mismatch"):
            validate_fundamental_signal_panel(store.con, "build")
    finally:
        store.connection.close()


def test_selected_label_evidence_covers_revision_order_and_stitching(tmp_path):
    store, day, as_of, signal = _warehouse(tmp_path)

    def evaluate(run_id):
        evaluate_fundamental_signals(store, FundamentalSignalEvaluationOptions(
            build_run_id="build", run_id=run_id, as_of_date=as_of,
            run_at=dt.datetime.combine(as_of, dt.time(22, 30), dt.UTC),
            label_source="labels",
        ))
        config_json, sample_sha = store.con.execute("""
            SELECT config_json,sample_sha256 FROM fundamental_signal_evaluation_runs
            WHERE run_id=?
        """, [run_id]).fetchone()
        selected_sha = store.con.execute("""
            SELECT selected_sha256 FROM fundamental_signal_evaluation_label_evidence
            WHERE run_id=? AND signal_id=? AND decision_date=? AND horizon_sessions=21
        """, [run_id, signal, day]).fetchone()[0]
        labeled = store.con.execute("""
            SELECT labeled_count FROM fundamental_signal_evaluation_deciles
            WHERE run_id=? AND signal_id=? AND decision_date=?
              AND horizon_sessions=21 AND decile=1
        """, [run_id, signal, day]).fetchone()[0]
        return json.loads(config_json), sample_sha, selected_sha, labeled

    try:
        baseline = evaluate("baseline")
        assert baseline[0]["evaluation_version"] == "fq2_v3"
        assert baseline[0]["label_evidence_version"] == "selected_label_v2"
        assert baseline[3] == 20

        store.con.execute("""
            UPDATE forward_returns_survivorship_safe
            SET source_loaded_at=source_loaded_at+INTERVAL 1 SECOND
            WHERE forward_return_id='L000_21'
        """)
        reordered = evaluate("revision_clock")
        assert reordered[1] != baseline[1]
        assert reordered[2] != baseline[2]
        assert reordered[3] == baseline[3]

        store.con.execute("""
            UPDATE forward_returns_survivorship_safe SET is_stitched=true
            WHERE forward_return_id='L000_21'
        """)
        stitched = evaluate("stitched")
        assert stitched[1] != reordered[1]
        assert stitched[2] != reordered[2]
        assert stitched[3] == 19
    finally:
        store.connection.close()


def test_newest_unsupported_basis_suppresses_older_adjusted_label(tmp_path):
    store, day, as_of, signal = _warehouse(tmp_path)
    try:
        store.con.execute("""
            INSERT INTO forward_returns_survivorship_safe
            (forward_return_id,source,security_id,as_of_date,horizon_days,
             forward_end_date,raw_forward_return,forward_return,
             is_delisted_in_horizon,is_stitched,
             available_at,price_basis,calculation_version)
            SELECT 'newer_raw','labels',security_id,as_of_date,horizon_days,
                   forward_end_date,raw_forward_return,forward_return,false,false,
                   available_at+INTERVAL 1 HOUR,'close','forward_return_publication_v2'
            FROM forward_returns_survivorship_safe WHERE forward_return_id='L000_21'
        """)
        evaluate_fundamental_signals(store, FundamentalSignalEvaluationOptions(
            build_run_id="build", run_id="raw_eval", as_of_date=as_of,
            run_at=dt.datetime.combine(as_of, dt.time(22, 30), dt.UTC),
            label_source="labels",
        ))
        assert store.con.execute("""
            SELECT eligible_count,labeled_count,unsupported_basis_count
            FROM fundamental_signal_evaluation_deciles
            WHERE run_id='raw_eval' AND signal_id=? AND decision_date=?
              AND horizon_sessions=21 AND decile=1
        """, [signal, day]).fetchone() == (20, 19, 1)
    finally:
        store.connection.close()


def test_malformed_latest_terminal_is_invalid_and_valid_policy_is_counted(tmp_path):
    store, day, as_of, signal = _warehouse(tmp_path)
    try:
        store.con.execute("""
            INSERT INTO forward_returns_survivorship_safe
            (forward_return_id,source,security_id,as_of_date,horizon_days,
             forward_end_date,raw_forward_return,terminal_return,forward_return,
             is_delisted_in_horizon,is_stitched,delist_date,terminal_return_source,
             return_observation_id,available_at,price_basis,calculation_version)
            SELECT 'malformed','labels',security_id,as_of_date,horizon_days,
                   forward_end_date,0,.5,.5,true,true,NULL,'observed','obs',
                   available_at+INTERVAL 1 HOUR,'adjusted_close','forward_return_publication_v2'
            FROM forward_returns_survivorship_safe WHERE forward_return_id='L000_21'
        """)
        store.con.execute("""
            INSERT INTO forward_returns_survivorship_safe
            (forward_return_id,source,security_id,as_of_date,horizon_days,
             forward_end_date,raw_forward_return,terminal_return,forward_return,
             is_delisted_in_horizon,is_stitched,delist_date,terminal_return_source,
             return_observation_id,available_at,price_basis,calculation_version)
            SELECT 'valid_policy','labels',security_id,as_of_date,horizon_days,
                   forward_end_date,0,-.5,-.5,true,true,as_of_date+INTERVAL 1 DAY,
                   'policy',NULL,available_at+INTERVAL 1 HOUR,
                   'adjusted_close','forward_return_publication_v2'
            FROM forward_returns_survivorship_safe WHERE forward_return_id='L001_21'
        """)
        evaluate_fundamental_signals(store, FundamentalSignalEvaluationOptions(
            build_run_id="build", run_id="terminal_eval", as_of_date=as_of,
            run_at=dt.datetime.combine(as_of, dt.time(22, 30), dt.UTC),
            label_source="labels",
        ))
        assert store.con.execute("""
            SELECT eligible_count,labeled_count,invalid_count,policy_terminal_count
            FROM fundamental_signal_evaluation_deciles
            WHERE run_id='terminal_eval' AND signal_id=? AND decision_date=?
              AND horizon_sessions=21 AND decile=1
        """, [signal, day]).fetchone() == (20, 19, 1, 1)
    finally:
        store.connection.close()


def _desk_q5(con, **params):
    sql = (Path(__file__).resolve().parents[1] / "sql/research/desk-q5-monthly-factor-portfolios.sql").read_text(
        encoding="utf-8")
    cursor = con.execute(sql, params)
    columns = [column[0] for column in cursor.description]
    return [dict(zip(columns, row, strict=True)) for row in cursor.fetchall()]


def test_fq2_v3_robust_p_is_r3a_and_desk_q5_gate_refuses_legacy_inference(tmp_path):
    store, _, as_of, signal = _warehouse(tmp_path)
    con = store.con
    try:
        # fq2_v3 p-values are R3a's robust_p_value on the same session-positioned spread series.
        con.execute("""
            INSERT INTO fundamental_signal_evaluation_deciles
            SELECT 'planted',?,(DATE '2024-01-01'+t::INTEGER),t,NULL,NULL,21,'holdout','evaluated',d,
                   200,0,20,20,0,0,0,0,0,0.0,CASE WHEN d=10 THEN 0.002+0.01*sin(t/5.0) ELSE 0.0 END
            FROM range(1,401) r(t),(VALUES (1),(10)) v(d) WHERE t%7<>0
        """, [signal])
        comparison = evaluation._summaries(con, "planted", (signal,))
        spreads = con.execute("""
            SELECT session_number,max(mean_forward_return) FILTER (WHERE decile=10)
                                  -max(mean_forward_return) FILTER (WHERE decile=1)
            FROM fundamental_signal_evaluation_deciles WHERE run_id='planted' GROUP BY 1 ORDER BY 1
        """).fetchall()
        positioned = np.full(spreads[-1][0] - spreads[0][0] + 1, np.nan)
        for session, spread in spreads:
            positioned[session - spreads[0][0]] = spread
        expected = mean_inference(positioned, horizon_periods=21)
        row = con.execute("""
            SELECT p_value,holm_p_value,z_statistic,hac_standard_error,hac_lags,gross_mean
            FROM fundamental_signal_evaluation_summaries
            WHERE run_id='planted' AND horizon_sessions=21 AND split='holdout'
        """).fetchone()
        assert row[0] == row[1] == expected.robust_p_value      # a one-signal Holm family
        assert row[2:5] == (expected.z_equivalent, expected.robust_standard_error, expected.robust_df)
        legacy = evaluation.calendar_hac_statistics(spreads, 21)
        assert row[5] == legacy["gross_mean"] and row[0] != legacy["p_value"]
        reported = comparison[f"{signal}|21|holdout"]
        assert (reported["nw_p_value"], reported["legacy_calendar_hac_p_value"]) == (
            expected.nw_p_value, legacy["p_value"])

        # The desk-Q5 gate reads only fq2_v3 robust Holm p; legacy results stay readable, labeled.
        evaluate_fundamental_signals(store, FundamentalSignalEvaluationOptions(
            build_run_id="build", run_id="eval", as_of_date=as_of,
            run_at=dt.datetime.combine(as_of, dt.time(22, 30), dt.UTC), label_source="labels"))
        config = json.loads(con.execute("SELECT config_json FROM fundamental_signal_evaluation_runs "
                                        "WHERE run_id='eval'").fetchone()[0])
        assert config["evaluation_version"] == evaluation.EVALUATION_VERSION == "fq2_v3"
        assert config["inference"]["columns"]["p_value"] == "robust_p_value"
        con.execute("UPDATE fundamental_signal_evaluation_summaries SET p_value=.001,holm_p_value=.004 "
                    "WHERE run_id='eval' AND split='holdout'")

        def gate(version):
            con.execute("UPDATE fundamental_signal_evaluation_runs SET config_json=json_merge_patch("
                        "config_json, json_object('evaluation_version', ?)) WHERE run_id='eval'", [version])
            rows = _desk_q5(con, cutoff=dt.datetime.combine(as_of, dt.time(23)), build_run_id="build",
                            evaluation_run_id="eval", signal_id=signal,
                            start_date=dt.date(2024, 3, 1), end_date=dt.date(2024, 3, 31))
            return {r["horizon_sessions"]: (r["status"], r["inference_status"], r["significance_gate_status"],
                                            r["significance_gate_pass"], r["robust_p_value"],
                                            r["legacy_overconfident_p_value"]) for r in rows}

        assert gate("fq2_v3") == {
            21: ("month_end_not_evaluated", "ewc_fixed_b_robust", "robust_holm_pass", True, .001, None),
            63: ("month_end_not_evaluated", "ewc_fixed_b_robust", "secondary_horizon_not_gated", False, .001, None)}
        legacy_rows = gate("fq2_v2")
        assert {h: r[:4] for h, r in legacy_rows.items()} == {
            h: ("month_end_not_evaluated", "inference_overconfident_legacy", "inference_overconfident_legacy", False)
            for h in (21, 63)}
        assert {r[4:] for r in legacy_rows.values()} == {(None, .001)}
        assert gate("fq2_v1")[21][:4] == ("unsupported_evaluation_contract", "inference_overconfident_legacy",
                                          "evaluation_contract_failed", False)
        assert evaluation.inference_status("fq2_v2") == evaluation.inference_status(None) == \
            "inference_overconfident_legacy"
    finally:
        store.connection.close()


def test_failed_run_remains_diagnostic_and_cannot_be_reused(tmp_path, monkeypatch):
    store, _, as_of, _ = _warehouse(tmp_path)
    options = FundamentalSignalEvaluationOptions(
        build_run_id="build", run_id="broken", as_of_date=as_of,
        run_at=dt.datetime.combine(as_of, dt.time(22, 30), dt.UTC),
        label_source="labels",
    )
    try:
        def fail_join(*_args):
            raise RuntimeError("synthetic failure")

        monkeypatch.setattr(evaluation, "_join_labels", fail_join)
        with pytest.raises(RuntimeError, match="synthetic failure"):
            evaluate_fundamental_signals(store, options)
        assert store.con.execute("""
            SELECT status,diagnostic_json FROM fundamental_signal_evaluation_runs
            WHERE run_id='broken'
        """).fetchone() == ("failed", '{"error_type":"RuntimeError"}')
        with pytest.raises(ValueError, match="already exists"):
            evaluate_fundamental_signals(store, options)
    finally:
        store.connection.close()
