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


# Content digests of the tiny fixture captured on the pre-refactor module
# (HEAD 80878e3e) before the shared calendar builder existed. panel_sha256 also
# hashes this file's bytes, so it changes with any edit; these components must not.
_FQ1_PINNED = {
    "first": {
        "spec": "8cc0d4e3d6c57bd5db0dd4e617d1b817233f89336e72d6495cee6ac723002a04",
        "definitions": "c370acee5cc4e30c72acd5ba33e8e0c83eb2cfd6bdae886eac2013dc9098bcd1",
        "calendar": "fec8f9864dfa96dcd4ca42bfdf9d630a01d54838f208cd7dc28d0d79a7b93625",
        "values": "f7d832b14a56f78e8c974938addbbb3f26ff31d33a4a6fadea024b661551eb6b",
        "inputs": "5f01178c19a0235f4c9e01f747b8c63300e842b1e289dc789f1e4138efc4f680",
        "coverage": "95fb5b347b42b7b0c22b53a74ae638247ec2059561dea70419f89dec415337c3",
        "proofs": "9bb0ba5bcfd347e1e4729ff659663964e49955baa16575041116c471e0905ab2",
    },
    "empty": {
        "spec": "8cc0d4e3d6c57bd5db0dd4e617d1b817233f89336e72d6495cee6ac723002a04",
        "definitions": "c370acee5cc4e30c72acd5ba33e8e0c83eb2cfd6bdae886eac2013dc9098bcd1",
        "calendar": "fec8f9864dfa96dcd4ca42bfdf9d630a01d54838f208cd7dc28d0d79a7b93625",
        "values": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "inputs": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "coverage": "681612c80fdc16a759206be7ada2d4586cb4d705fe905ecfe00a5a2e6e5c55aa",
        "proofs": "9bb0ba5bcfd347e1e4729ff659663964e49955baa16575041116c471e0905ab2",
    },
}
_FQ1_COMPONENT_QUERIES = {
    "values": """SELECT signal_id,decision_date,security_id,decision_at,entry_date,score,
        eligible,reason,input_end,input_available_at,cohort_size
        FROM fundamental_signal_values WHERE run_id=? ORDER BY signal_id,decision_date,security_id""",
    "inputs": """SELECT signal_id,decision_date,security_id,term_ordinal,metric_code,metric_window,
        weight,derived_value_id,derived_owner_security_id,definition_hash,inputs_hash,
        selected_input_refs_hash,lineage_digest,selected_input_cik,lineage_status,
        lineage_leaf_ids_json,selected_leaf_oldest_end,selected_leaf_newest_end,period_end,
        fiscal_period_start,fiscal_period_end,available_at,value_origin,history_status,
        value_status,raw_value,reason
        FROM fundamental_signal_inputs WHERE run_id=?
        ORDER BY signal_id,decision_date,security_id,term_ordinal""",
    "coverage": """SELECT signal_id,decision_date,decision_at,entry_date,status,visible_members,
        common_members,overlap_members,qualified_cik,unmatched_owner_states,eligible_scores,
        excluded_scores,constant_scores,reasons_json
        FROM fundamental_signal_coverage WHERE run_id=? ORDER BY signal_id,decision_date""",
    "proofs": """SELECT derived_value_id,derived_owner_security_id,metric_code,metric_window,
        root_available_at,selected_input_refs_hash,selected_cik,status,reason,leaf_ids_json,
        input_clocks_json,fiscal_ends_json,proof_digest,oldest_fiscal_end,newest_fiscal_end,
        latest_input_clock,proof_cutoff
        FROM fundamental_signal_proofs WHERE run_id=? ORDER BY derived_value_id""",
}


def _fq1_components(con, run_id):
    spec, definitions, calendar = con.execute("""
        SELECT spec_sha256, definitions_sha256, calendar_sha256
        FROM fundamental_signal_runs WHERE run_id=?
    """, [run_id]).fetchone()
    return {"spec": spec, "definitions": definitions, "calendar": calendar,
            **{name: fs._stream_digest(con, query, [run_id])
               for name, query in _FQ1_COMPONENT_QUERIES.items()}}


def test_shared_builder_keeps_fq1_panel_content_byte_identical(tiny_panel_store):
    con = tiny_panel_store.con
    assert build_fundamental_signal_panel(tiny_panel_store, _options("first")).status == "complete"
    assert _fq1_components(con, "first") == _FQ1_PINNED["first"]
    con.execute("DELETE FROM universe_us_listed_membership")
    assert build_fundamental_signal_panel(tiny_panel_store, _options("empty")).status == "blocked_empty"
    assert _fq1_components(con, "empty") == _FQ1_PINNED["empty"]


def test_calendar_and_metric_batch_selection_equals_single_session_legs(tiny_panel_store):
    con = tiny_panel_store.con
    _, _, hashes = fs._definitions(con, canonical_signals(None))
    days = [(dt.date(2024, 1, 2), dt.datetime(2024, 1, 2, 22)),
            (dt.date(2024, 1, 3), dt.datetime(2024, 1, 3, 22))]
    metrics = [("eps_diluted_q_growth_yoy", "q"), ("total_accruals", "ttm")]
    single = []
    for day, cutoff in days:
        fs._stage_cohort(con, day, cutoff, day)
        for code, window in metrics:
            _stage_leg(con, "single", code, window, hashes[(code, window)], hashes, day, cutoff, 200)
            single += con.execute("""
                SELECT ?, ?, security_id, cohort_reason, derived_value_id, raw_value, reason,
                       lineage_digest, selected_input_cik, age_days, max_age_days
                FROM _fs_leg
            """, [day, code]).fetchall()
    fs.stage_calendar(con, days, table="_t_calendar")
    counts = fs.stage_cohort(con, calendar_table="_t_calendar")
    assert counts == {day: (3, 3, 0, 3) for day, _ in days}
    fs.stage_metric_legs(con, [fs.MetricLeg(code, window, hashes[(code, window)], 200, 200,
                                            code == "eps_diluted_q_growth_yoy")
                               for code, window in metrics], table="_t_metrics")
    fs.stage_selected_states(con, run_id="multi", all_hashes=hashes, calendar_table="_t_calendar",
                             cohort_table="_fs_cohort_cal", metrics_table="_t_metrics")
    multi = con.execute("""
        SELECT decision_date, metric_code, security_id, cohort_reason, derived_value_id, raw_value,
               reason, lineage_digest, selected_input_cik, age_days, max_age_days
        FROM _fs_leg
    """).fetchall()
    assert sorted(multi, key=repr) == sorted(single, key=repr)
    assert {row[6] for row in multi} >= {"valid", "stale_current_anchor"}
    # One proof per selected state across both sessions and both metrics.
    assert con.execute("""
        SELECT count(*), count(DISTINCT derived_value_id) FROM fundamental_signal_proofs
        WHERE run_id='multi'
    """).fetchone() == con.execute("""
        SELECT count(*), count(*) FROM fundamental_signal_proofs WHERE run_id='single'
    """).fetchone()


def test_reconstructed_cohort_uses_owner_links_visible_at_the_cutoff():
    con = duckdb.connect(":memory:")
    con.execute("""
        CREATE TABLE universe_us_listed_membership (
          universe_id VARCHAR, source VARCHAR, security_id VARCHAR, symbol VARCHAR,
          security_type VARCHAR, exchange_code VARCHAR, cik VARCHAR, reason VARCHAR,
          valid_from DATE, valid_to DATE, available_at TIMESTAMP, as_of_date DATE);
        CREATE TABLE security_identifier_history (
          security_id VARCHAR, id_type VARCHAR, id_value VARCHAR, valid_from DATE,
          valid_to DATE, available_at TIMESTAMP, as_of_date DATE);
        CREATE TEMP TABLE _links (
          price_security_id VARCHAR, cik VARCHAR, valid_from DATE, valid_to DATE,
          available_at TIMESTAMP, identity_basis VARCHAR, link_method VARCHAR,
          unlinked_reason VARCHAR);
    """)
    members = [
        ("p1", "common", "XNAS", "0000000001", "member"),
        ("p2", "common_unverified", "XNYS", None, "member_no_cik"),
        ("p3", "unknown", "UNKNOWN", None, "reconstructed_no_listing_evidence"),
        ("p4", "common", "XNAS", "0000000009", "member"),
        ("p5", "ADR", "XNYS", "0000000005", "member"),
        ("p6", "common", "XNAS", "0000000006", "member"),
        ("p7", "common", "XNAS", None, "member"),
        ("p8", "common", "XNAS", None, "member"),
        ("p9", "unknown", "UNKNOWN", None, "reconstructed_no_listing_evidence"),
    ]
    for security, kind, venue, cik, reason in members:
        con.execute("""
            INSERT INTO universe_us_listed_membership VALUES
            ('us_listed_reconstructed_v1','atx-db us-listed universe builder',?,?,?,?,?,?,
             '2020-01-02',NULL,'2020-01-02 22:00:00','2020-01-02')
        """, [security, security.upper(), kind, venue, cik, reason])
    linked = "current_ticker_unverified"
    for row in [
        ("p1", "0000000001", "2020-01-02", None, "2020-01-02 22:00:00", linked, "current_sec_ticker", None),
        ("p2", "0000000002", "2020-01-02", None, "2020-01-02 22:00:00", linked, "current_sec_ticker", None),
        ("p3", None, None, None, None, None, None, "no_current_ticker"),
        ("p4", "0000000004", "2020-01-02", None, "2020-01-02 22:00:00", linked, "current_sec_ticker", None),
        ("p5", "0000000005", "2020-01-02", None, "2020-01-02 22:00:00", linked, "current_sec_ticker", None),
        # Link interval covers the day but its modeled clock is after the cutoff.
        ("p6", "0000000006", "2021-06-30", None, "2021-07-01 22:00:00", linked, "current_sec_ticker", None),
        ("p7", "0000000007", "2020-01-02", None, "2020-01-02 22:00:00", linked, "current_sec_ticker", None),
        ("p7", "0000000077", "2020-01-02", None, "2020-01-02 22:00:00", linked, "cik_security_id", None),
        ("p9", "0000000019", "2020-01-02", None, "2020-01-02 22:00:00", linked, "cik_security_id", None),
    ]:
        con.execute("INSERT INTO _links VALUES (?,?,?,?,?,?,?,?)", list(row))
    day, cutoff = dt.date(2021, 6, 30), dt.datetime(2021, 6, 30, 22)
    counts = fs._stage_cohort(con, day, cutoff, day,
                              universe_id=fs.RECONSTRUCTED_UNIVERSE_ID,
                              identity_basis=fs.IDENTITY_BASIS_RECONSTRUCTED,
                              owner_links_table="_links")
    reasons = dict(con.execute("""
        SELECT security_id, cohort_reason || ':' || coalesce(owner_link_reason, cik)
        FROM _fs_cohort_cal
    """).fetchall())
    assert reasons == {
        "p1": "valid:0000000001",
        "p2": "valid:0000000002",
        "p3": "missing_owner_link:no_current_ticker",
        "p4": "membership_cik_mismatch:0000000004",
        "p5": "not_common:0000000005",
        "p6": "missing_owner_link:owner_link_after_cutoff",
        "p7": "ambiguous_owner_link:0000000077",
        "p8": "missing_owner_link:no_owner_link_interval",
        "p9": "valid:0000000019",
    }
    assert counts == (9, 8, 0, 3)
    assert con.execute("SELECT DISTINCT identity_basis FROM _fs_cohort_cal").fetchall() == [(linked,)]
    # The strict default never reads the reconstructed universe or the links.
    assert fs._stage_cohort(con, day, cutoff, day) == (0, 0, 0, 0)
    with pytest.raises(ValueError, match="owner-link relation"):
        fs.stage_cohort(con, identity_basis=fs.IDENTITY_BASIS_RECONSTRUCTED)


def test_absent_historical_cohort_has_per_date_diagnostics(tiny_panel_store):
    tiny_panel_store.con.execute("DELETE FROM universe_us_listed_membership")
    result = build_fundamental_signal_panel(tiny_panel_store, _options("empty"))
    assert result.status == "blocked_empty"
    assert result.eligible_values == 0
    assert tiny_panel_store.con.execute("""
      SELECT count(*),min(status),max(status)
      FROM fundamental_signal_coverage WHERE run_id='empty'
    """).fetchone() == (10, "empty_membership", "empty_membership")
