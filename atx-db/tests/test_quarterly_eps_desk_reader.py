"""Tiny real-IQ2 checks of the EPS consumer's PIT and refusal contracts."""

from __future__ import annotations

import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path

import duckdb
import pytest

from atx_db.derived_lineage import registered_definition_hashes
from atx_db.derived_registry import DERIVED_SOURCE_NAME, default_derived_definitions

spec = importlib.util.spec_from_file_location(
    "eps_desk_reader", Path(__file__).resolve().parents[1] / "scripts/read_quarterly_eps_growth.py",
)
assert spec is not None and spec.loader is not None
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)
CIK = "0000000123"
OWNER = "actual-source-owner"


@pytest.fixture
def con():
    with duckdb.connect(":memory:", config={"memory_limit": "256MB", "threads": "1"}) as db:
        db.execute("""
            CREATE TABLE fundamental_fact_revisions (
                security_id VARCHAR,cik VARCHAR,as_of_date DATE,available_at TIMESTAMP,source_loaded_at TIMESTAMP);
            CREATE TABLE derived_metric_definitions (
                metric_code VARCHAR,expression VARCHAR,metric_window VARCHAR,version VARCHAR,inputs_json VARCHAR);
            CREATE TABLE fundamental_standardized (
                standardized_id VARCHAR,canonical_code VARCHAR,cik VARCHAR,basis VARCHAR,source VARCHAR,
                period_start DATE,period_end DATE,available_at TIMESTAMP,value DOUBLE);
            CREATE TABLE derived_metric_values (
                derived_value_id VARCHAR,revision_group_id VARCHAR,security_id VARCHAR,metric_code VARCHAR,
                metric_window VARCHAR,period_end DATE,value DOUBLE,value_status VARCHAR,inputs_hash VARCHAR,
                as_of_date DATE,available_at TIMESTAMP,source_loaded_at TIMESTAMP,source VARCHAR,valid_to TIMESTAMP,
                definition_hash VARCHAR,target_bucket BIGINT,fiscal_period_start DATE,fiscal_period_end DATE,
                history_status VARCHAR,selected_input_refs_json VARCHAR,selected_input_refs_hash VARCHAR);
        """)
        db.execute("INSERT INTO fundamental_fact_revisions VALUES (?,?, '2025-01-01','2025-01-01','2025-01-01')",
                   [OWNER, CIK])
        for definition in default_derived_definitions():
            if definition.metric_code not in reader.METRICS.values():
                continue
            db.execute("INSERT INTO derived_metric_definitions VALUES (?,?,?,?,?)", [
                definition.metric_code, definition.expression, definition.window, definition.version,
                json.dumps(list(definition.inputs), separators=(",", ":")),
            ])
        yield db


LEAF_CODES = {"diluted": "eps_diluted__1035", "basic": "eps_basic__1034"}


def seed(db, *, period_end="2026-06-30", value=1.0, prior=1.0, current=2.0, leaf_cik=CIK, measure="diluted"):
    metric, code = reader.METRICS[measure], LEAF_CODES[measure]
    tag = "" if measure == "diluted" else f"{measure}-"
    end = dt.date.fromisoformat(period_end)
    prior_end = end.replace(year=end.year - 1)  # Explicitly a calendar-year fixture.
    bucket = (end.year * 12 + end.month) // 3
    at = dt.datetime(2026, 8, 1)
    refs = []
    for role, leaf_end, amount, offset in (("current", end, current, 0), ("prior", prior_end, prior, 4)):
        leaf = f"{role}-{tag}{end}"
        db.execute("INSERT INTO fundamental_standardized VALUES (?,?,?,?,?,?,?,?,?)", [
            leaf, code, leaf_cik, "quarterly", "statement", None, leaf_end, at, amount,
        ])
        refs.append({"kind": "item", "code": code, "bucket": bucket - offset,
                     "offset": offset, "status": "selected", "state_id": leaf,
                     "available_at": at.isoformat(), "cik": leaf_cik, "basis": "quarterly",
                     "source": "statement", "period_start": None, "period_end": leaf_end.isoformat()})
    payload = json.dumps({"version": 1, "refs": refs}, separators=(",", ":"))
    db.execute("INSERT INTO derived_metric_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", [
        f"root-{tag}{end}", f"eps-{tag}{end}", OWNER, metric, "q", end, value,
        "valid" if value is not None else "missing_input_or_domain", "frame", at.date(), at, at,
        DERIVED_SOURCE_NAME, None, registered_definition_hashes(db)[(metric, "q")],
        bucket, None, end, "event_reconstructed", payload, hashlib.sha256(payload.encode()).hexdigest(),
    ])


def read(db, **overrides):
    kwargs = {"cik": "123", "content_as_of": dt.datetime(2026, 9, 20, 22, tzinfo=dt.UTC),
              "start": dt.date(2025, 10, 1), "end": dt.date(2026, 7, 1), "latest": 3}
    kwargs.update(overrides)
    return reader.read_quarterly_eps(db, **kwargs)


def test_latest_periods_retain_null_revision_instead_of_resurrecting_numeric_state(con):
    for end in ("2025-12-31", "2026-03-31", "2026-06-30"):
        seed(con, period_end=end)
    con.execute("""
        INSERT INTO derived_metric_values
        SELECT * REPLACE ('new-null' AS derived_value_id,NULL AS value,'missing_input_or_domain' AS value_status,
                          TIMESTAMP '2026-08-20' AS available_at,DATE '2026-08-20' AS as_of_date)
        FROM derived_metric_values WHERE derived_value_id='root-2026-06-30'
    """)
    before = read(con, content_as_of=dt.datetime(2026, 8, 10, tzinfo=dt.UTC))
    assert before["status"] == "qualified_numeric_observations"
    assert [row["growth_percent"] for row in before["rows"]] == [100.0] * 3
    latest = read(con)
    assert latest["status"] == "selected_states_unavailable"
    assert latest["rows"][0]["period_end"] == dt.date(2026, 6, 30)
    assert latest["rows"][0]["growth_percent"] is None
    assert latest["rows"][0]["value_status"] == "missing_input_or_domain"
    assert latest["issuer_diagnostics"]["issuer_market_association"]["historical_security_qualified"] is False


def test_selected_foreign_cik_is_never_returned_as_issuer_numeric_growth(con):
    seed(con, leaf_cik="0000000456")
    result = read(con, latest=1)
    assert result["rows"] == []
    assert result["status"] == "no_qualified_states"
    assert result["issuer_diagnostics"]["derived_lineage_rejected_count"] == 1
    assert result["issuer_diagnostics"]["derived_lineage_diagnostics"][0]["reason"] == "mismatch"


@pytest.mark.parametrize(("prior", "value", "status", "percent"), [
    (0.0, None, "selected_states_unavailable", None),
    (-1.0, 3.0, "qualified_numeric_observations", 300.0),
])
def test_zero_and_negative_base_canonical_states_are_preserved(con, prior, value, status, percent):
    seed(con, value=value, prior=prior)
    result = read(con, latest=1)
    assert result["status"] == status
    assert result["rows"][0]["value"] == value
    assert result["rows"][0]["growth_percent"] == percent


def test_basic_measure_reads_only_its_own_growth_series(con):
    for end in ("2025-12-31", "2026-03-31", "2026-06-30"):
        seed(con, period_end=end)
    diluted_only = read(con, measure="basic")
    # No basic materialization: the diluted series is never substituted.
    assert diluted_only["rows"] == []
    assert diluted_only["status"] == "materialization_missing_for_request"
    assert (diluted_only["measure"], diluted_only["metric"]) == ("basic", "eps_basic_q_growth_yoy")

    for end in ("2025-12-31", "2026-03-31", "2026-06-30"):
        seed(con, period_end=end, measure="basic", value=0.5, prior=2.0, current=3.0)
    basic = read(con, measure="basic")
    assert basic["status"] == "qualified_numeric_observations"
    assert [row["growth_percent"] for row in basic["rows"]] == [50.0] * 3
    assert all(row["revision_group_id"].startswith("eps-basic-") for row in basic["rows"])
    diluted = read(con)
    assert (diluted["measure"], diluted["metric"]) == ("diluted", "eps_diluted_q_growth_yoy")
    assert [row["growth_percent"] for row in diluted["rows"]] == [100.0] * 3
    with pytest.raises(ValueError, match="measure"):
        read(con, measure="adjusted")


def test_missing_migration_and_missing_materialization_are_distinct(con):
    empty = read(con)
    assert empty["status"] == "materialization_missing_for_request"
    con.execute("ALTER TABLE derived_metric_values DROP selected_input_refs_json")
    con.execute("ALTER TABLE derived_metric_values DROP selected_input_refs_hash")
    missing = read(con)
    assert missing["status"] == "schema_prerequisite_missing"
    assert missing["rows"] == []
    assert missing["missing_columns"] == {"derived_metric_values": [
        "selected_input_refs_hash", "selected_input_refs_json",
    ]}


def test_owner_payload_bound_is_checked_before_service_fetch(con, monkeypatch):
    con.execute("UPDATE fundamental_fact_revisions SET security_id=?", ["x" * 70_000])

    def forbidden(*args, **kwargs):
        pytest.fail("issuer payload fetched before byte refusal")

    monkeypatch.setattr(reader._SnapshotService, "issuer_content_range", forbidden)
    with pytest.raises(ValueError, match="preflight exceeds bound"):
        read(con)
