"""Focused contracts for the daily fundamental research panel."""

from __future__ import annotations

import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import duckdb
import pytest

from atx_db import _derived_pit as pit
from atx_db import fundamental_signal_research as fs
from atx_db.connection import DuckDBStore
from atx_db.derived_registry import DerivedMetricDefinition
from atx_db.fundamental_signal_research import (
    FundamentalSignalResearchOptions,
    _qualify_proof_batch,
    _stage_leg,
    _validate_options,
    build_fundamental_signal_panel,
    canonical_signals,
    default_signals,
    validate_fundamental_signal_panel,
)
from atx_db.migrations.bodies_0324 import create_fundamental_signal_tables


def test_default_family_is_frozen_and_complete_case_rank_mix():
    specs = canonical_signals(None)
    assert len(specs) == 5
    mix = next(row for row in specs if row["signal_id"] == "fundamental_rank_mix")
    assert len(mix["terms"]) == 4
    assert all(term["transform"] == "cs_rank" for term in mix["terms"])
    assert sorted(term["weight"] for term in mix["terms"]) == [-0.25, -0.25, 0.25, 0.25]
    assert specs == canonical_signals(reversed(default_signals()))


@pytest.mark.parametrize("bad", [
    [{"signal_id": "cash", "terms": [{"metric_code": "revenue", "metric_window": "ttm",
        "weight": 1, "transform": "identity"}]}],
    [{"signal_id": "bad", "terms": [{"metric_code": "total_accruals", "metric_window": "ttm",
        "weight": float("nan"), "transform": "identity"}]}],
    [{"signal_id": "bad", "terms": [{"metric_code": "total_accruals", "metric_window": "ttm",
        "weight": 1, "transform": "sql"}]}],
])
def test_specs_reject_currency_nonfinite_and_arbitrary_sql(bad):
    with pytest.raises(ValueError):
        canonical_signals(bad)


def test_explicit_utc_and_final_decision_required():
    base = dict(start_date=dt.date(2024, 1, 2), end_date=dt.date(2024, 1, 3),
                as_of_date=dt.date(2024, 1, 3), run_id="research_1")
    with pytest.raises(ValueError, match="timezone-aware"):
        _validate_options(FundamentalSignalResearchOptions(
            **base, run_at=dt.datetime(2024, 1, 4)))
    with pytest.raises(ValueError, match="22:00"):
        _validate_options(FundamentalSignalResearchOptions(
            **base, run_at=dt.datetime(2024, 1, 3, 21, tzinfo=dt.UTC)))
    assert _validate_options(FundamentalSignalResearchOptions(
        **base, run_at=dt.datetime(2024, 1, 3, 22, tzinfo=dt.UTC)))[0] == \
        dt.datetime(2024, 1, 3, 22)


def test_minimal_table_creation_replays_and_cli_requires_dates():
    con = duckdb.connect(":memory:")
    create_fundamental_signal_tables(con)
    create_fundamental_signal_tables(con)
    names = {row[0] for row in con.execute("""
      SELECT table_name FROM information_schema.tables
      WHERE table_name LIKE 'fundamental_signal_%'
    """).fetchall()}
    assert names == {
        "fundamental_signal_runs", "fundamental_signal_definitions",
        "fundamental_signal_values", "fundamental_signal_inputs",
        "fundamental_signal_proofs", "fundamental_signal_coverage",
    }
    script = Path(__file__).resolve().parents[1] / "scripts" / "research_fundamental_signals.py"
    spec = importlib.util.spec_from_file_location("fq1_cli_contract", script)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with pytest.raises(SystemExit):
        module.parser().parse_args(["build", "--db-path", "x.duckdb"])


def test_aggregate_resolver_cap_splits_without_erasing_per_root_failure():
    calls = []
    def resolver(_con, ids, **_kwargs):
        calls.append(tuple(ids))
        if len(ids) > 2:
            return {root: SimpleNamespace(status="limit_exceeded",
                                          reason="metric node bound") for root in ids}
        return {root: SimpleNamespace(
            status="limit_exceeded" if root == "bad" else "qualified",
            reason="depth or node bound" if root == "bad" else "ok",
        ) for root in ids}
    result = _qualify_proof_batch(None, ["a", "b", "bad", "c"], {}, resolver)
    assert [len(ids) for ids in calls] == [4, 2, 2]
    assert result["a"].status == "qualified"
    assert result["bad"].status == "limit_exceeded"
    assert result["bad"].reason == "depth or node bound"


def test_visible_event_selection_keeps_null_invalidation_and_late_filing():
    con = duckdb.connect(":memory:")
    con.execute("""
        CREATE TEMP TABLE _fs_cohort (
          security_id VARCHAR, cik VARCHAR, cohort_reason VARCHAR);
        INSERT INTO _fs_cohort VALUES ('a','0000000001','valid'),
                                      ('b','0000000002','valid');
        CREATE TABLE derived_metric_values (
          derived_value_id VARCHAR, source VARCHAR, security_id VARCHAR,
          metric_code VARCHAR, metric_window VARCHAR, period_end DATE,
          value DOUBLE, available_at TIMESTAMP, as_of_date DATE,
          target_bucket BIGINT, definition_hash VARCHAR, inputs_hash VARCHAR,
          fiscal_period_start DATE, fiscal_period_end DATE, value_origin VARCHAR,
          history_status VARCHAR, value_status VARCHAR, valid_to TIMESTAMP,
          selected_input_refs_hash VARCHAR);
        CREATE TABLE fundamental_signal_proofs (
          run_id VARCHAR,derived_value_id VARCHAR,derived_owner_security_id VARCHAR,
          metric_code VARCHAR,metric_window VARCHAR,root_available_at TIMESTAMP,
          selected_input_refs_hash VARCHAR,selected_cik VARCHAR,status VARCHAR,
          reason VARCHAR,leaf_ids_json VARCHAR,input_clocks_json VARCHAR,
          fiscal_ends_json VARCHAR,proof_digest VARCHAR,oldest_fiscal_end DATE,
          newest_fiscal_end DATE,latest_input_clock TIMESTAMP,proof_cutoff TIMESTAMP);
    """)
    valid_hash = "a" * 64
    rows = [
        ("numeric", "a", dt.date(2023, 12, 31), 0.2, dt.datetime(2024, 1, 2, 10),
         dt.date(2023, 12, 31), "valid"),
        ("null", "a", dt.date(2023, 12, 31), None, dt.datetime(2024, 1, 3, 10),
         dt.date(2023, 12, 31), "missing_input_or_domain"),
        ("late", "b", dt.date(2023, 12, 31), -0.1, dt.datetime(2024, 1, 4, 10),
         dt.date(2023, 12, 31), "valid"),
    ]
    for state_id, owner, period_end, value, clock, fiscal_end, status in rows:
        con.execute("""
            INSERT INTO derived_metric_values VALUES
              (?, 'atx-db declarative derived metrics v1', ?, 'total_accruals','ttm',
               ?,?,?,?,8096,?,?,NULL,?,'quarterly','event_reconstructed',?,NULL,?)
        """, [state_id, owner, period_end, value, clock, clock.date(),
              valid_hash, "b" * 64, fiscal_end, status, "c" * 64])
        con.execute("""
          INSERT INTO fundamental_signal_proofs VALUES
          ('r',?,?, 'total_accruals','ttm',?,?,?, 'qualified','ok',
           '[]','[]','[]',?, ?, ?, ?, ?)
        """, [state_id, owner, clock, "c" * 64,
              "0000000001" if owner == "a" else "0000000002",
              "d" * 64, fiscal_end, fiscal_end, clock, clock])
    _stage_leg(con, "r", "total_accruals", "ttm", valid_hash,
               {("total_accruals", "ttm"): valid_hash},
               dt.date(2024, 1, 2), dt.datetime(2024, 1, 2, 22), 200)
    assert con.execute("SELECT derived_value_id FROM _fs_leg WHERE security_id='a'").fetchone()[0] == "numeric"
    assert con.execute("SELECT derived_value_id FROM _fs_leg WHERE security_id='b'").fetchone()[0] is None
    _stage_leg(con, "r", "total_accruals", "ttm", valid_hash,
               {("total_accruals", "ttm"): valid_hash},
               dt.date(2024, 1, 3), dt.datetime(2024, 1, 3, 22), 200)
    assert con.execute("SELECT derived_value_id,reason FROM _fs_leg WHERE security_id='a'").fetchone() == \
        ("null", "invalid_current_state")
    assert con.execute("SELECT derived_value_id FROM _fs_leg WHERE security_id='b'").fetchone()[0] is None


def test_prior_verified_owner_cik_suppresses_other_owner_after_17_unproved_revisions(
    monkeypatch,
):
    con = duckdb.connect(":memory:")
    con.execute("""
        CREATE TEMP TABLE _fs_cohort (security_id VARCHAR,cik VARCHAR,cohort_reason VARCHAR);
        INSERT INTO _fs_cohort VALUES ('listed_x','0000000001','valid');
        CREATE TABLE derived_metric_values (
          derived_value_id VARCHAR,source VARCHAR,security_id VARCHAR,metric_code VARCHAR,
          metric_window VARCHAR,period_end DATE,value DOUBLE,available_at TIMESTAMP,
          as_of_date DATE,target_bucket BIGINT,definition_hash VARCHAR,inputs_hash VARCHAR,
          fiscal_period_start DATE,fiscal_period_end DATE,value_origin VARCHAR,
          history_status VARCHAR,value_status VARCHAR,valid_to TIMESTAMP,
          selected_input_refs_hash VARCHAR);
        CREATE TABLE fundamental_signal_proofs (
          run_id VARCHAR,derived_value_id VARCHAR,derived_owner_security_id VARCHAR,
          metric_code VARCHAR,metric_window VARCHAR,root_available_at TIMESTAMP,
          selected_input_refs_hash VARCHAR,selected_cik VARCHAR,status VARCHAR,
          reason VARCHAR,leaf_ids_json VARCHAR,input_clocks_json VARCHAR,
          fiscal_ends_json VARCHAR,proof_digest VARCHAR,oldest_fiscal_end DATE,
          newest_fiscal_end DATE,latest_input_clock TIMESTAMP,proof_cutoff TIMESTAMP);
    """)
    publisher = "atx-db declarative derived metrics v1"
    entries = [
        ("a_old", "issuer_a", 0.3, "2024-01-02 10:00:00", "valid", "0000000001", "qualified"),
        ("a_null", "issuer_a", None, "2024-01-03 10:00:00", "missing_input_or_domain", None, "missing"),
        ("b_old", "issuer_b", 0.1, "2024-01-02 09:00:00", "valid", "0000000001", "qualified"),
    ]
    for state_id, owner, value, clock, status, cik, proof_status in entries:
        con.execute("""
          INSERT INTO derived_metric_values VALUES
          (?,?,?,?,?,'2023-12-31',?,?,CAST(? AS DATE),8096,?,?,NULL,'2023-12-31',
           'quarterly','event_reconstructed',?,NULL,?)
        """, [state_id, publisher, owner, "total_accruals", "ttm", value, clock,
              clock, "a" * 64, "b" * 64, status, "c" * 64])
        if state_id == "a_old":
            continue
        con.execute("""
          INSERT INTO fundamental_signal_proofs VALUES
          ('r',?,?, 'total_accruals','ttm',?,?,?,?,?,'[]','[]','[]',?,
           '2023-12-31','2023-12-31',?,?)
        """, [state_id, owner, clock, "c" * 64, cik, proof_status,
              "verified" if cik else "missing selected leaf", "d" * 64 if cik else None,
              clock, clock])
    for index in range(17):
        clock = dt.datetime(2024, 1, 2, 10, index + 1)
        con.execute("""
          INSERT INTO derived_metric_values VALUES
          (?,?,?,?,?,'2023-12-31',NULL,?,CAST(? AS DATE),8096,?,?,NULL,'2023-12-31',
           'quarterly','event_reconstructed','missing_input_or_domain',NULL,?)
        """, [f"a_missing_{index}", publisher, "issuer_a", "total_accruals", "ttm",
              clock, clock, "a" * 64, "b" * 64, "c" * 64])

    from atx_db import derived_lineage

    def resolve(_con, ids, **_kwargs):
        return {
            state_id: derived_lineage.LineageQualification(
                "qualified" if state_id == "a_old" else "missing",
                "verified" if state_id == "a_old" else "no selected leaf",
                "0000000001" if state_id == "a_old" else None,
                ("a_old_leaf",) if state_id == "a_old" else (),
                ("0000000001",) if state_id == "a_old" else (),
                (dt.datetime(2024, 1, 2, 9),) if state_id == "a_old" else (),
                (dt.date(2023, 12, 31),) if state_id == "a_old" else (),
                "d" * 64 if state_id == "a_old" else None,
            ) for state_id in ids
        }

    monkeypatch.setattr(derived_lineage, "qualify_selected_lineage", resolve)
    _stage_leg(con, "r", "total_accruals", "ttm", "a" * 64,
               {("total_accruals", "ttm"): "a" * 64},
               dt.date(2024, 1, 3), dt.datetime(2024, 1, 3, 22), 200)
    assert con.execute("SELECT reason FROM _fs_leg WHERE security_id='listed_x'").fetchone()[0] == \
        "ambiguous_derived_owner"


@pytest.fixture
def tiny_panel_store(monkeypatch):
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(":memory:")
    con = store.con
    con.execute("SET TimeZone='UTC'")
    create_fundamental_signal_tables(con)
    con.execute("""
        CREATE TABLE derived_metric_definitions (
          metric_code VARCHAR,metric_window VARCHAR,expression VARCHAR,
          inputs_json VARCHAR,version VARCHAR);
        CREATE TABLE derived_metric_values (
          derived_value_id VARCHAR,source VARCHAR,security_id VARCHAR,
          metric_code VARCHAR,metric_window VARCHAR,target_bucket BIGINT,
          period_end DATE,value DOUBLE,available_at TIMESTAMP,valid_to TIMESTAMP,
          inputs_hash VARCHAR,definition_hash VARCHAR,history_status VARCHAR,
          value_status VARCHAR,selected_input_refs_json VARCHAR,
          selected_input_refs_hash VARCHAR,as_of_date DATE,fiscal_period_start DATE,
          fiscal_period_end DATE,value_origin VARCHAR);
        CREATE TABLE fundamental_standardized (
          standardized_id VARCHAR,canonical_code VARCHAR,cik VARCHAR,basis VARCHAR,
          source VARCHAR,period_start DATE,period_end DATE,available_at TIMESTAMP,
          value DOUBLE);
        CREATE TABLE universe_us_listed_membership (
          universe_id VARCHAR,source VARCHAR,security_id VARCHAR,security_type VARCHAR,
          exchange_code VARCHAR,cik VARCHAR,valid_from DATE,valid_to DATE,
          available_at TIMESTAMP,as_of_date DATE);
        CREATE TABLE security_identifier_history (
          security_id VARCHAR,id_type VARCHAR,id_value VARCHAR,valid_from DATE,
          valid_to DATE,available_at TIMESTAMP,as_of_date DATE);
        CREATE TABLE market_daily_metrics (
          source VARCHAR,trade_date DATE,available_at TIMESTAMP,as_of_date DATE,
          close DOUBLE);
    """)
    defs = tuple(DerivedMetricDefinition(
        metric_code=code, family="test", expression="x", window=window,
        inputs=("item:x",), requires_market=False, description="test", version="1",
    ) for code, window in sorted(fs.DIMENSIONLESS))
    monkeypatch.setattr(fs, "default_derived_definitions", lambda: defs)
    hashes = {(definition.metric_code, definition.window): pit.definition_hash(definition)
              for definition in defs}
    for definition in defs:
        con.execute("INSERT INTO derived_metric_definitions VALUES (?,?,?,?,?)",
                    [definition.metric_code, definition.window, definition.expression,
                     '["item:x"]', definition.version])
    for security_id, cik in (("a", "1"), ("b", "2"), ("c", "3")):
        con.execute("""
          INSERT INTO universe_us_listed_membership VALUES
          ('us_listed_v1','atx-db us-listed universe builder',?,'common','XNAS',?,
           '2020-01-01',NULL,'2023-01-01 00:00:00','2023-01-01')
        """, [security_id, cik])
        con.execute("""
          INSERT INTO security_identifier_history VALUES
          (?,'CIK',?,'2020-01-01',NULL,'2023-01-01 00:00:00','2023-01-01')
        """, [security_id, cik])
    for day in ("2024-01-02", "2024-01-03", "2024-01-04"):
        con.execute("""
          INSERT INTO market_daily_metrics VALUES
          ('atx-db daily market panel v1',?,CAST(? AS TIMESTAMP) + INTERVAL 21 HOUR,?,100.0)
        """, [day, day, day])

    def add_root(owner, code, window, value, state_id, clock, fiscal_end="2023-12-31",
                 status="valid", prior_yoy=False):
        fiscal_date = dt.date.fromisoformat(fiscal_end)
        bucket = (fiscal_date.year * 12 + fiscal_date.month - 1 +
                  int(fiscal_date.day >= 15)) // 3
        cik = str({"a": 1, "b": 2, "c": 3}[owner])
        leaf_clock = dt.datetime.fromisoformat(clock) - dt.timedelta(hours=1)
        periods = [(fiscal_date, 0)]
        if prior_yoy:
            periods.append((fiscal_date.replace(year=fiscal_date.year - 1), 4))
        refs = []
        for period, offset in periods:
            leaf_id = f"leaf_{state_id}_{offset}"
            con.execute("""
              INSERT INTO fundamental_standardized VALUES
              (?,'x',?,'quarterly','test',NULL,?,?,1.0)
            """, [leaf_id, cik, period, leaf_clock])
            refs.append({"kind": "item", "code": "x", "bucket": bucket - offset,
                         "offset": offset, "status": "selected", "state_id": leaf_id,
                         "available_at": leaf_clock.isoformat(), "cik": cik,
                         "basis": "quarterly", "source": "test",
                         "period_start": None, "period_end": period.isoformat()})
        payload = json.dumps({"version": 1, "refs": refs}, sort_keys=True, separators=(",", ":"))
        con.execute("""
          INSERT INTO derived_metric_values VALUES
          (?, 'atx-db declarative derived metrics v1',?,?,?,?,?,?,?,?,?,?,'event_reconstructed',
           ?,?,?,CAST(? AS DATE),NULL,?,'quarterly')
        """, [state_id, owner, code, window, bucket, fiscal_date, value,
              dt.datetime.fromisoformat(clock), None, "b" * 64,
              hashes[(code, window)], status, payload,
              hashlib.sha256(payload.encode()).hexdigest(), clock, fiscal_date])

    for owner in ("a", "b"):
        for code, window in sorted(fs.DIMENSIONLESS):
            value = -0.1 if code in ("total_accruals", "net_debt_ebitda") else 0.2
            add_root(owner, code, window, value, f"{owner}_{code}",
                     "2024-01-02T18:00:00", prior_yoy=code.endswith("yoy"))
    add_root("c", "eps_diluted_q_growth_yoy", "q", 0.2, "c_stale",
             "2024-01-02T18:00:00", fiscal_end="2023-05-31", prior_yoy=True)
    # The later NULL state replaces a's numeric EPS state in the same bucket.
    con.execute("""
      UPDATE derived_metric_values SET valid_to='2024-01-03 18:00:00'
      WHERE derived_value_id='a_eps_diluted_q_growth_yoy'
    """)
    add_root("a", "eps_diluted_q_growth_yoy", "q", None, "a_eps_null",
             "2024-01-03T18:00:00", status="missing_input_or_domain", prior_yoy=True)
    try:
        yield store
    finally:
        store.connection.close()
        store.connection = None


def _options(run_id):
    return FundamentalSignalResearchOptions(
        start_date=dt.date(2024, 1, 2), end_date=dt.date(2024, 1, 3),
        as_of_date=dt.date(2024, 1, 4),
        run_at=dt.datetime(2024, 1, 4, 22, tzinfo=dt.UTC), run_id=run_id,
    )


def test_tiny_build_seals_pit_values_and_repeatable_digest(tiny_panel_store):
    first = build_fundamental_signal_panel(tiny_panel_store, _options("first"))
    assert first.status == "complete"
    assert first.sessions == 2
    assert first.eligible_values > 0
    con = tiny_panel_store.con
    assert con.execute("""
      SELECT score FROM fundamental_signal_values
      WHERE run_id='first' AND signal_id='fundamental_rank_mix'
        AND decision_date='2024-01-02' AND security_id='a'
    """).fetchone()[0] == pytest.approx(0.5)
    assert con.execute("""
      SELECT score FROM fundamental_signal_values
      WHERE run_id='first' AND signal_id='fundamental_rank_mix'
        AND decision_date='2024-01-02' AND security_id='b'
    """).fetchone()[0] == pytest.approx(0.5)
    assert con.execute("""
      SELECT reason FROM fundamental_signal_inputs
      WHERE run_id='first' AND signal_id='eps_growth_yoy'
        AND decision_date='2024-01-03' AND security_id='a'
    """).fetchone()[0] != "valid"
    assert con.execute("""
      SELECT reason FROM fundamental_signal_inputs
      WHERE run_id='first' AND signal_id='eps_growth_yoy'
        AND decision_date='2024-01-02' AND security_id='c'
    """).fetchone()[0] == "stale_current_anchor"
    assert con.execute("""
      SELECT count(*) FROM fundamental_signal_coverage WHERE run_id='first'
    """).fetchone()[0] == 10
    second = build_fundamental_signal_panel(tiny_panel_store, _options("second"))
    assert second.status == "complete"
    assert second.panel_sha256 == first.panel_sha256
    verified = validate_fundamental_signal_panel(con, "first")
    assert verified.panel_sha256 == first.panel_sha256
    assert verified.sessions == (
        (dt.date(2024, 1, 2), dt.date(2024, 1, 3)),
        (dt.date(2024, 1, 3), dt.date(2024, 1, 4)),
    )
    con.execute("""
      UPDATE fundamental_signal_values SET score=score+1
      WHERE run_id='first' AND signal_id='fundamental_rank_mix'
        AND decision_date='2024-01-02' AND security_id='a'
    """)
    with pytest.raises(ValueError, match="digest mismatch"):
        validate_fundamental_signal_panel(con, "first")
    original = con.execute("""
      SELECT diagnostic_json,blockers_json FROM fundamental_signal_runs
      WHERE run_id='second'
    """).fetchone()
    changed = json.loads(original[0])
    changed["common_membership_rows"] += 1
    con.execute("UPDATE fundamental_signal_runs SET diagnostic_json=? WHERE run_id='second'",
                [json.dumps(changed, sort_keys=True, separators=(",", ":"))])
    with pytest.raises(ValueError, match="diagnostic counts changed"):
        validate_fundamental_signal_panel(con, "second")
    con.execute("UPDATE fundamental_signal_runs SET diagnostic_json=? WHERE run_id='second'",
                [original[0]])
    con.execute("UPDATE fundamental_signal_runs SET blockers_json=? WHERE run_id='second'",
                [json.dumps([*json.loads(original[1]), "unmatched_derived_owner_lineage"],
                            sort_keys=True, separators=(",", ":"))])
    with pytest.raises(ValueError, match="blockers changed"):
        validate_fundamental_signal_panel(con, "second")


def test_failed_partial_run_remains_unsealed(tiny_panel_store, monkeypatch):
    real_publish = fs._publish_signal
    def fail_on_second_date(*args, **kwargs):
        if args[3] == dt.date(2024, 1, 3):
            raise RuntimeError("injected partition failure")
        return real_publish(*args, **kwargs)
    monkeypatch.setattr(fs, "_publish_signal", fail_on_second_date)
    with pytest.raises(RuntimeError, match="injected partition failure"):
        build_fundamental_signal_panel(tiny_panel_store, _options("failed"))
    assert tiny_panel_store.con.execute("""
      SELECT status,panel_sha256 FROM fundamental_signal_runs WHERE run_id='failed'
    """).fetchone() == ("failed", None)
    with pytest.raises(ValueError, match="not complete"):
        validate_fundamental_signal_panel(tiny_panel_store.con, "failed")
    assert tiny_panel_store.con.execute("""
      SELECT count(*) FROM fundamental_signal_coverage WHERE run_id='failed'
    """).fetchone()[0] == 5


def test_absent_historical_cohort_has_per_date_diagnostics(tiny_panel_store):
    tiny_panel_store.con.execute("DELETE FROM universe_us_listed_membership")
    result = build_fundamental_signal_panel(tiny_panel_store, _options("empty"))
    assert result.status == "blocked_empty"
    assert result.eligible_values == 0
    assert tiny_panel_store.con.execute("""
      SELECT count(*),min(status),max(status)
      FROM fundamental_signal_coverage WHERE run_id='empty'
    """).fetchone() == (10, "empty_membership", "empty_membership")
