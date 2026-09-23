"""Selected operand and recursive issuer qualification on temporary warehouses."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace

import duckdb
import pytest

from atx_db import derived_metrics as engine
from atx_db._derived_annual import plan_for
from atx_db._derived_pit import definition_hash
from atx_db.derived_lineage import qualify_selected_lineage
from atx_db.derived_registry import DerivedMetricDefinition
from atx_db.migrations import bodies_0323


def definition(code, expression, inputs, window="q"):
    return DerivedMetricDefinition(code, "rollup", expression, window, tuple(inputs), False, "lineage", "1")


DEFINITIONS = (
    definition("sales_q", "revenue", ("item:revenue",)),
    definition("sales_double", "sales_q * 2", ("metric:sales_q",)),
    definition("sales_lag", "lag(revenue, 1)", ("item:revenue",)),
    definition("sales_ttm", "ttm(revenue)", ("item:revenue",), "ttm"),
    definition("chosen", "coalesce(revenue, cost_of_revenue_cogs)",
               ("item:revenue", "item:cost_of_revenue_cogs")),
)


@pytest.fixture
def store(monkeypatch):
    con = duckdb.connect(":memory:")
    con.execute("SET memory_limit='256MB'")
    con.execute("SET threads=1")
    con.execute("""
        CREATE TABLE fundamental_standardized (
            standardized_id VARCHAR PRIMARY KEY, source VARCHAR, security_id VARCHAR,
            cik VARCHAR, item_id INTEGER, canonical_code VARCHAR, basis VARCHAR,
            period_start DATE, period_end DATE, value DOUBLE, as_of_date DATE,
            available_at TIMESTAMP, input_codes_json VARCHAR, input_item_ids_json VARCHAR,
            rule_id VARCHAR, combination_rule VARCHAR, revision_sequence INTEGER,
            is_latest_revision BOOLEAN
        )
    """)
    con.execute("""
        CREATE TABLE derived_metric_values (
            derived_value_id VARCHAR PRIMARY KEY, source VARCHAR, security_id VARCHAR,
            metric_code VARCHAR, metric_window VARCHAR, period_end DATE, value DOUBLE,
            available_at TIMESTAMP, inputs_hash VARCHAR, as_of_date DATE,
            is_latest_revision BOOLEAN, run_id VARCHAR, value_status VARCHAR,
            revision_group_id VARCHAR, revision_sequence INTEGER, revision_count INTEGER,
            valid_to TIMESTAMP, target_bucket BIGINT, definition_hash VARCHAR,
            arithmetic_available_at TIMESTAMP, history_status VARCHAR,
            value_origin VARCHAR, fiscal_period_start DATE, fiscal_period_end DATE,
            selected_input_refs_json VARCHAR, selected_input_refs_hash VARCHAR
        )
    """)
    store = SimpleNamespace(con=con, path=Path(":memory:"), initialize=lambda: None)
    monkeypatch.setattr(engine, "default_derived_definitions", lambda: DEFINITIONS)
    try:
        yield store
    finally:
        con.close()


def fact(store, code, end, value, at, *, cik="0000001234", basis="quarterly",
         start=None, identity=None):
    identity = identity or f"{code}|{basis}|{end}|{at}"
    store.con.execute("""
        INSERT INTO fundamental_standardized (
            standardized_id, source, security_id, cik, item_id, canonical_code, basis,
            period_start, period_end, value, as_of_date, available_at,
            input_codes_json, input_item_ids_json, rule_id, combination_rule,
            revision_sequence, is_latest_revision
        ) VALUES (?, 'test', 'S1', ?, 1, ?, ?, CAST(? AS DATE), CAST(? AS DATE), ?,
                  CAST(? AS DATE), CAST(? AS TIMESTAMP), '[]', '[]', 'r', 'direct', 1, true)
    """, [identity, cik, code, basis, start, end, value, at, at])
    return identity


def current(store, code, end="2024-12-31"):
    row = store.con.execute("""
        SELECT derived_value_id, value, selected_input_refs_json, selected_input_refs_hash
        FROM derived_metric_values WHERE metric_code=? AND period_end=CAST(? AS DATE)
        ORDER BY available_at DESC, derived_value_id DESC LIMIT 1
    """, [code, end]).fetchone()
    assert row is not None
    return row


def expected_hashes():
    by_code = {d.metric_code: d for d in DEFINITIONS}
    return {(d.metric_code, d.window): definition_hash(d, plan_for(d, by_code)) for d in DEFINITIONS}


def refs(row):
    assert hashlib.sha256(row[2].encode()).hexdigest() == row[3]
    payload = json.loads(row[2])
    assert payload["version"] == 1
    return payload["refs"]


def test_direct_nested_coalesce_and_cross_cik(store):
    revenue = fact(store, "revenue", "2024-12-31", 100, "2025-02-10 12:00:00")
    unused = fact(store, "cost_of_revenue_cogs", "2024-12-31", 30,
                  "2025-02-10 12:00:00", cik="0000009999")
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",), event_chunk_size=1))
    chosen = current(store, "chosen")
    assert chosen[1] == 100
    assert [ref["state_id"] for ref in refs(chosen)] == [revenue]
    assert unused not in chosen[2]
    nested = current(store, "sales_double")
    assert len(refs(nested)) == 1 and refs(nested)[0]["kind"] == "metric"
    proof = qualify_selected_lineage(store.con, [chosen[0], nested[0]],
                                      expected_definition_hashes=expected_hashes())
    assert {item.status for item in proof.values()} == {"qualified"}
    assert all(item.selected_cik == "0000001234" and item.leaf_ids == (revenue,)
               for item in proof.values())
    wrong = qualify_selected_lineage(store.con, [nested[0]], expected_cik="9999",
                                     expected_definition_hashes=expected_hashes())
    assert wrong[nested[0]].status == "mismatch"


def test_ttm_refs_revision_and_deterministic_chunks(store):
    ids = []
    for end, at, value in (
        ("2024-03-31", "2024-05-10 12:00:00", 10),
        ("2024-06-30", "2024-08-10 12:00:00", 20),
        ("2024-09-30", "2024-11-10 12:00:00", 30),
        ("2024-12-31", "2025-02-10 12:00:00", 40),
    ):
        ids.append(fact(store, "revenue", end, value, at))
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",), event_chunk_size=1))
    ttm = current(store, "sales_ttm")
    assert ttm[1] == 100
    assert {ref["state_id"] for ref in refs(ttm)} == set(ids)
    before = store.con.execute("""
        SELECT derived_value_id, selected_input_refs_json, selected_input_refs_hash
        FROM derived_metric_values ORDER BY derived_value_id
    """).fetchall()
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",), event_chunk_size=128))
    after = store.con.execute("""
        SELECT derived_value_id, selected_input_refs_json, selected_input_refs_hash
        FROM derived_metric_values ORDER BY derived_value_id
    """).fetchall()
    assert after == before
    amended = fact(store, "revenue", "2024-06-30", 20, "2025-03-01 12:00:00",
                   identity="same-value-new-source")
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",)))
    revised = current(store, "sales_ttm")
    assert revised[1] == 100 and amended in {ref["state_id"] for ref in refs(revised)}
    assert revised[3] != ttm[3]


def test_missing_tamper_and_scope_limit(store):
    fact(store, "revenue", "2024-12-31", 100, "2025-02-10 12:00:00")
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",)))
    original = current(store, "sales_q")
    with pytest.raises(RuntimeError, match="selected-ref row exceeds"):
        engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(
            security_ids=("S1",), max_selected_ref_bytes=16))
    assert current(store, "sales_q") == original
    store.con.execute("UPDATE derived_metric_values SET selected_input_refs_json='{}' WHERE derived_value_id=?",
                      [original[0]])
    proof = qualify_selected_lineage(store.con, [original[0]], expected_definition_hashes=expected_hashes())
    assert proof[original[0]].status == "invalid"
    store.con.execute("UPDATE derived_metric_values SET selected_input_refs_json=NULL, selected_input_refs_hash=NULL "
                      "WHERE derived_value_id=?", [original[0]])
    proof = qualify_selected_lineage(store.con, [original[0]], expected_definition_hashes=expected_hashes())
    assert proof[original[0]].status == "legacy_unverifiable"


def test_annual_fallback_excludes_unused_annual_candidate(store):
    # Complete FY candidate for sales_ttm; another annual code has a different
    # CIK but cannot become a selected leaf of the revenue-only formula.
    chosen = fact(store, "revenue", "2024-12-31", 100, "2025-02-10 12:00:00",
                  basis="annual", start="2024-01-01")
    unused = fact(store, "cost_of_revenue_cogs", "2024-12-31", 30,
                  "2025-02-10 12:00:00", cik="0000009999", basis="annual",
                  start="2024-01-01")
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",)))
    row = current(store, "sales_ttm")
    assert row[1] == 100
    assert [ref["state_id"] for ref in refs(row)] == [chosen]
    assert unused not in row[2]


def test_null_invalidation_retains_owner_evidence(store):
    missing = fact(store, "revenue", "2024-12-31", None, "2025-02-10 12:00:00")
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",)))
    row = current(store, "sales_q")
    assert row[1] is None
    assert refs(row)[0]["status"] == "missing"
    assert refs(row)[0]["state_id"] == missing
    proof = qualify_selected_lineage(store.con, [row[0]], expected_definition_hashes=expected_hashes())
    assert proof[row[0]].status == "invalid"
    assert proof[row[0]].selected_cik == "0000001234"


def test_minimal_0322_to_0323_upgrade_keeps_legacy_null(monkeypatch):
    con = duckdb.connect(":memory:")
    try:
        con.execute("CREATE TABLE derived_metric_values (derived_value_id VARCHAR, inputs_hash VARCHAR)")
        con.execute("INSERT INTO derived_metric_values VALUES ('old', 'opaque')")
        con.execute("CREATE TABLE table_catalog (table_name VARCHAR, pit_notes VARCHAR, updated_at TIMESTAMP)")
        con.execute("INSERT INTO table_catalog VALUES ('derived_metric_values', 'old note', now())")
        con.execute("CREATE TABLE field_catalog (table_name VARCHAR, field_name VARCHAR, description VARCHAR, updated_at TIMESTAMP)")

        def catalog_fields(connection, tables):
            assert tables == ("derived_metric_values",)
            connection.execute("""
                INSERT INTO field_catalog VALUES
                    ('derived_metric_values', 'selected_input_refs_json', NULL, now()),
                    ('derived_metric_values', 'selected_input_refs_hash', NULL, now())
            """)

        monkeypatch.setattr(bodies_0323, "_catalog_fields_for_tables", catalog_fields)
        monkeypatch.setattr(bodies_0323, "_refresh_schema_contract_v2_pin", lambda connection: None)
        bodies_0323.MIGRATIONS[0].up(con)
        assert con.execute("""
            SELECT selected_input_refs_json, selected_input_refs_hash
            FROM derived_metric_values WHERE derived_value_id='old'
        """).fetchone() == (None, None)
        assert con.execute("""
            SELECT count(*) FROM field_catalog WHERE table_name='derived_metric_values'
              AND description IS NOT NULL
        """).fetchone() == (2,)
    finally:
        con.close()


def test_missing_cycle_and_clock_fail_closed(store):
    fact(store, "revenue", "2024-12-31", 100, "2025-02-10 12:00:00")
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",)))
    root = current(store, "sales_double")
    before = dt.datetime(2025, 2, 9)
    proof = qualify_selected_lineage(store.con, [root[0]], decision_cutoff=before,
                                      expected_definition_hashes=expected_hashes())
    assert proof[root[0]].status == "invalid"

    def replace_ref(ref):
        payload = json.dumps({"version": 1, "refs": [ref]}, separators=(",", ":"))
        store.con.execute("""
            UPDATE derived_metric_values SET selected_input_refs_json=?, selected_input_refs_hash=?
            WHERE derived_value_id=?
        """, [payload, hashlib.sha256(payload.encode()).hexdigest(), root[0]])

    missing = dict(refs(root)[0], state_id="absent-dependency")
    replace_ref(missing)
    proof = qualify_selected_lineage(store.con, [root[0]], expected_definition_hashes=expected_hashes())
    assert proof[root[0]].status == "missing"

    own = store.con.execute("""
        SELECT metric_code, target_bucket, available_at, inputs_hash, definition_hash
        FROM derived_metric_values WHERE derived_value_id=?
    """, [root[0]]).fetchone()
    cycle = dict(refs(root)[0], code=own[0], bucket=own[1], offset=0,
                 state_id=root[0], available_at=own[2].isoformat(),
                 inputs_hash=own[3], definition_hash=own[4])
    replace_ref(cycle)
    proof = qualify_selected_lineage(store.con, [root[0]], expected_definition_hashes=expected_hashes())
    assert proof[root[0]].status == "invalid" and "cyclic" in proof[root[0]].reason


def test_actual_cross_cik_winning_leaf_rejected(store):
    fact(store, "revenue", "2024-12-31", None, "2025-02-10 12:00:00")
    foreign = fact(store, "cost_of_revenue_cogs", "2024-12-31", 30,
                   "2025-02-10 12:00:00", cik="0000009999")
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",)))
    root = current(store, "chosen")
    assert [ref["state_id"] for ref in refs(root)] == [foreign]
    proof = qualify_selected_lineage(store.con, [root[0]], expected_cik="1234",
                                      expected_definition_hashes=expected_hashes())
    assert proof[root[0]].status == "mismatch"


def test_metric_ref_source_and_span_are_pinned(store):
    fact(store, "revenue", "2024-12-31", 100, "2025-02-10 12:00:00")
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",)))
    root = current(store, "sales_double")
    original_ref = refs(root)[0]
    for field, bad in (("source", "forged"), ("period_end", "1900-01-01")):
        altered = dict(original_ref, **{field: bad})
        payload = json.dumps({"version": 1, "refs": [altered]}, separators=(",", ":"))
        store.con.execute("""
            UPDATE derived_metric_values SET selected_input_refs_json=?, selected_input_refs_hash=?
            WHERE derived_value_id=?
        """, [payload, hashlib.sha256(payload.encode()).hexdigest(), root[0]])
        proof = qualify_selected_lineage(store.con, [root[0]], expected_definition_hashes=expected_hashes())
        assert proof[root[0]].status == "invalid"
        assert "mismatched metric ref" in proof[root[0]].reason


def test_lag_ref_selects_prior_bucket(store):
    old = fact(store, "revenue", "2024-09-30", 30, "2024-11-10 12:00:00")
    fact(store, "revenue", "2024-12-31", 40, "2025-02-10 12:00:00")
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",)))
    row = current(store, "sales_lag")
    assert row[1] == 30
    selected = refs(row)
    assert len(selected) == 1 and selected[0]["state_id"] == old
    assert selected[0]["offset"] == 1


def test_weighted_share_branch_selects_annual_state(store, monkeypatch):
    weighted = definition("weighted_ratio", "safe_div(common_equity, weighted_avg_shares_basic)",
                          ("item:common_equity", "item:weighted_avg_shares_basic"), "ttm")
    monkeypatch.setattr(engine, "default_derived_definitions", lambda: (weighted,))
    fact(store, "common_equity", "2024-12-31", 100, "2025-02-10 12:00:00", basis="instant")
    unused = fact(store, "weighted_avg_shares_basic", "2024-12-31", None,
                  "2025-02-10 12:00:00")
    annual_id = fact(store, "weighted_avg_shares_basic", "2024-12-31", 20,
                     "2025-02-10 12:00:00", basis="annual", start="2024-01-01")
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",)))
    row = current(store, "weighted_ratio")
    assert row[1] == 5
    ids = {ref["state_id"] for ref in refs(row)}
    assert annual_id in ids and unused not in ids
