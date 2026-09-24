"""Tiny exact-CIK serving proof; no warehouse bootstrap."""

from __future__ import annotations

import datetime as dt
import hashlib
import json

import duckdb
import pytest

from atx_db.api.service import ApiQueryError
from atx_db.asof.fundamentals import issuer_derived_asof
from atx_db.derived_lineage import qualify_issuer_derived_page, registered_definition_hashes
from atx_db.derived_registry import DERIVED_SOURCE_NAME, default_derived_definitions

from .test_issuer_content_query import CIK_A, CIK_B, OWNER_SAFE, _service


@pytest.fixture(autouse=True)
def _bounded_duckdb_connections(monkeypatch) -> None:
    original_connect = duckdb.connect

    def bounded_connect(*args, **kwargs):
        connection = original_connect(*args, **kwargs)
        connection.execute("SET memory_limit='256MB'")
        connection.execute("SET threads=1")
        return connection

    monkeypatch.setattr(duckdb, "connect", bounded_connect)


def _lineage_service(tmp_path):
    service = _service(tmp_path)
    definition = next(d for d in default_derived_definitions() if d.metric_code == "capex_q")
    path = tmp_path / "issuer-content.duckdb"
    with duckdb.connect(str(path)) as con:
        con.execute(
            "INSERT INTO derived_metric_definitions VALUES (?,?,?,?,?)",
            [definition.metric_code, definition.expression, definition.window,
             definition.version, json.dumps(list(definition.inputs), separators=(",", ":"))],
        )
        definition_hash = registered_definition_hashes(con)[("capex_q", "q")]
        con.execute(
            "CREATE TABLE fundamental_standardized (standardized_id VARCHAR, canonical_code VARCHAR, "
            "cik VARCHAR, basis VARCHAR, source VARCHAR, period_start DATE, period_end DATE, "
            "available_at TIMESTAMP, value DOUBLE)"
        )
        for suffix, owner_cik, period_end, leaf_at, root_at in (
            ("foreign", CIK_B, dt.date(2026, 3, 31), dt.datetime(2026, 4, 1), dt.datetime(2026, 5, 1)),
            ("owned", CIK_A, dt.date(2026, 6, 30), dt.datetime(2026, 7, 1), dt.datetime(2026, 8, 1)),
        ):
            bucket = (period_end.year * 12 + period_end.month - 1 + 1) // 3
            leaf_id = f"leaf-{suffix}"
            root_id = f"root-{suffix}"
            con.execute(
                "INSERT INTO fundamental_standardized VALUES (?,?,?,?,?,?,?,?,?)",
                [leaf_id, "capex__1305", owner_cik, "quarterly", "statement", None,
                 period_end, leaf_at, 20.0],
            )
            refs = json.dumps({"version": 1, "refs": [{
                "kind": "item", "code": "capex__1305", "bucket": bucket,
                "offset": 0, "status": "selected", "state_id": leaf_id,
                "available_at": leaf_at.isoformat(), "cik": owner_cik,
                "basis": "quarterly", "source": "statement", "period_start": None,
                "period_end": period_end.isoformat(),
            }]}, separators=(",", ":"))
            con.execute(
                "INSERT INTO derived_metric_values "
                "(derived_value_id, revision_group_id, security_id, metric_code, metric_window, "
                "period_end, value, value_status, inputs_hash, as_of_date, available_at, "
                "source_loaded_at, run_id, source, valid_to, definition_hash, target_bucket, "
                "fiscal_period_start, fiscal_period_end, history_status, "
                "selected_input_refs_json, selected_input_refs_hash) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                [root_id, root_id, OWNER_SAFE, "capex_q", "q", period_end, 20.0,
                 "valid", "frame", root_at.date(), root_at, root_at, "seed",
                 DERIVED_SOURCE_NAME, dt.datetime(2026, 8, 15), definition_hash,
                 bucket, None, period_end, "event_reconstructed", refs,
                 hashlib.sha256(refs.encode()).hexdigest()],
            )
            if suffix == "owned":
                # The whole newer NULL state wins latest; first_reported must
                # still prove the expired older root at its own event clock.
                later = dt.datetime(2026, 8, 20)
                con.execute(
                    "INSERT INTO derived_metric_values "
                    "(derived_value_id, revision_group_id, security_id, metric_code, metric_window, "
                    "period_end, value, value_status, inputs_hash, as_of_date, available_at, "
                    "source_loaded_at, run_id, source, valid_to, definition_hash, target_bucket, "
                    "fiscal_period_start, fiscal_period_end, history_status, "
                    "selected_input_refs_json, selected_input_refs_hash) "
                    "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    ["root-owned-null", root_id, OWNER_SAFE, "capex_q", "q", period_end,
                     None, "missing_input_or_domain", "frame", later.date(), later,
                     later, "seed", DERIVED_SOURCE_NAME, None, definition_hash, bucket,
                     None, period_end, "event_reconstructed", refs,
                     hashlib.sha256(refs.encode()).hexdigest()],
                )

        # More than a page of rejected states precedes the eligible Q2 state.
        con.executemany(
            "INSERT INTO derived_metric_values (derived_value_id, revision_group_id, "
            "security_id, metric_code, metric_window, period_end, value, value_status, "
            "inputs_hash, as_of_date, available_at, source_loaded_at, run_id) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
            [(f"legacy-{index:02d}", f"legacy-{index:02d}", OWNER_SAFE, "capex_q", "q",
              dt.date(2026, 3, 31), 900.0, "valid", "legacy", dt.date(2026, 5, 1),
              dt.datetime(2026, 5, 1), dt.datetime(2026, 5, 1), "seed")
             for index in range(64)],
        )
    return service, path


def test_selected_leaf_cik_and_first_event_clock_control_api_and_asof(tmp_path) -> None:
    service, path = _lineage_service(tmp_path)

    result = service.issuer_content_range(
        schema_name="derived-metrics", cik=CIK_A, start=dt.date(2026, 1, 1),
        end=dt.date(2026, 9, 1), content_as_of=dt.datetime(2026, 9, 1),
        items=["capex_q"], fields=["period_end", "value", "cik"],
        vintage="first_reported", limit=1,
    )
    assert result.data == [{"period_end": dt.date(2026, 6, 30), "value": 20.0, "cik": CIK_A}]
    assert result.metadata["derived_lineage_rejected_count"] == 65
    assert result.metadata["derived_lineage_scanned_count"] > 64
    assert {d["reason"] for d in result.metadata["derived_lineage_diagnostics"]} == {
        "mismatch", "legacy_unverifiable",
    }

    latest = service.issuer_content_range(
        schema_name="derived-metrics", cik=CIK_A, start=dt.date(2026, 1, 1),
        end=dt.date(2026, 9, 1), content_as_of=dt.datetime(2026, 9, 1),
        items=["capex_q"], fields=["period_end", "value", "value_status"], limit=1,
    )
    assert latest.data == [{"period_end": dt.date(2026, 6, 30), "value": None,
                            "value_status": "missing_input_or_domain"}]

    asof = issuer_derived_asof(CIK_A, dt.datetime(2026, 9, 1), path)
    assert "root-owned-null" in set(asof["derived_value_id"])
    assert "root-owned" not in set(asof["derived_value_id"])
    assert "root-foreign" not in set(asof["derived_value_id"])
    assert asof.attrs["derived_lineage_rejected_count"] >= 1


def test_dependency_foreign_leaf_is_rejected_at_both_readers(tmp_path) -> None:
    service, path = _lineage_service(tmp_path)
    q1 = dt.date(2026, 3, 31)
    q3 = dt.date(2026, 9, 30)
    child_at = dt.datetime(2026, 5, 1)
    root_at = dt.datetime(2026, 10, 1)
    with duckdb.connect(str(path)) as con:
        con.execute("UPDATE derived_metric_values SET valid_to=NULL WHERE derived_value_id='root-foreign'")
        definition_hash = registered_definition_hashes(con)[("capex_q", "q")]
        child_bucket = (q1.year * 12 + q1.month - 1 + 1) // 3
        root_bucket = (q3.year * 12 + q3.month - 1 + 1) // 3
        refs = json.dumps({"version": 1, "refs": [{
            "kind": "metric", "code": "capex_q", "bucket": child_bucket,
            "offset": root_bucket - child_bucket, "status": "selected",
            "state_id": "root-foreign", "available_at": child_at.isoformat(),
            "source": DERIVED_SOURCE_NAME, "period_start": None,
            "period_end": q1.isoformat(), "inputs_hash": "frame",
            "definition_hash": definition_hash,
        }]}, separators=(",", ":"))
        con.execute(
            "INSERT INTO derived_metric_values "
            "(derived_value_id, revision_group_id, security_id, metric_code, metric_window, "
            "period_end, value, value_status, inputs_hash, as_of_date, available_at, "
            "source_loaded_at, run_id, source, valid_to, definition_hash, target_bucket, "
            "fiscal_period_start, fiscal_period_end, history_status, "
            "selected_input_refs_json, selected_input_refs_hash) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            ["root-dependency", "root-dependency", OWNER_SAFE, "capex_q", "q", q3,
             30.0, "valid", "frame", root_at.date(), root_at, root_at, "seed",
             DERIVED_SOURCE_NAME, None, definition_hash, root_bucket, None, q3,
             "event_reconstructed", refs, hashlib.sha256(refs.encode()).hexdigest()],
        )
    result = service.issuer_content_range(
        schema_name="derived-metrics", cik=CIK_A, start=q3, end=dt.date(2026, 10, 1),
        content_as_of=dt.datetime(2026, 11, 1), items=["capex_q"],
        fields=["period_end", "value"],
    )
    assert result.data == []
    assert result.metadata["derived_lineage_diagnostics"][0]["reason"] == "mismatch"
    asof = issuer_derived_asof(CIK_A, dt.datetime(2026, 11, 1), path)
    assert "root-dependency" not in set(asof["derived_value_id"])

    # Replace the dependency with A's Q2 state. It expires after this root's
    # event but before the query cutoff, so event-time proof must still pass.
    with duckdb.connect(str(path)) as con:
        con.execute("DELETE FROM derived_metric_values WHERE derived_value_id='root-owned-null'")
        con.execute("UPDATE derived_metric_values SET valid_to=? "
                    "WHERE derived_value_id='root-owned'", [dt.datetime(2026, 10, 15)])
        child_end = dt.date(2026, 6, 30)
        child_bucket = (child_end.year * 12 + child_end.month - 1 + 1) // 3
        refs = json.dumps({"version": 1, "refs": [{
            "kind": "metric", "code": "capex_q", "bucket": child_bucket,
            "offset": root_bucket - child_bucket, "status": "selected",
            "state_id": "root-owned", "available_at": dt.datetime(2026, 8, 1).isoformat(),
            "source": DERIVED_SOURCE_NAME, "period_start": None,
            "period_end": child_end.isoformat(), "inputs_hash": "frame",
            "definition_hash": definition_hash,
        }]}, separators=(",", ":"))
        con.execute("UPDATE derived_metric_values SET selected_input_refs_json=?, "
                    "selected_input_refs_hash=? WHERE derived_value_id='root-dependency'",
                    [refs, hashlib.sha256(refs.encode()).hexdigest()])
    qualified = service.issuer_content_range(
        schema_name="derived-metrics", cik=CIK_A, start=q3, end=dt.date(2026, 10, 1),
        content_as_of=dt.datetime(2026, 11, 1), items=["capex_q"],
        fields=["period_end", "value"],
    )
    assert qualified.data == [{"period_end": q3, "value": 30.0}]
    assert "root-dependency" in set(
        issuer_derived_asof(CIK_A, dt.datetime(2026, 11, 1), path)["derived_value_id"]
    )


@pytest.mark.parametrize("tamper", ["source", "definition_hash", "clock"])
def test_tampered_selected_lineage_is_excluded_and_empty_asof_keeps_columns(tmp_path, tamper) -> None:
    service, path = _lineage_service(tmp_path)
    with duckdb.connect(str(path)) as con:
        if tamper == "clock":
            con.execute("UPDATE fundamental_standardized SET available_at=? "
                        "WHERE standardized_id='leaf-owned'", [dt.datetime(2026, 8, 25)])
        else:
            con.execute(f"UPDATE derived_metric_values SET {tamper}=? "
                        "WHERE derived_value_id='root-owned-null'", ["tampered"])
    result = service.issuer_content_range(
        schema_name="derived-metrics", cik=CIK_A,
        start=dt.date(2026, 6, 1), end=dt.date(2026, 9, 1),
        content_as_of=dt.datetime(2026, 9, 1), items=["capex_q"],
        fields=["period_end", "value"],
    )
    assert result.data == []
    assert result.metadata["derived_lineage_rejected_count"] == 1
    asof = issuer_derived_asof(CIK_A, dt.datetime(2026, 9, 1), path)
    assert asof.empty
    assert "derived_value_id" in asof.columns
    assert asof.attrs["derived_lineage_rejected_count"] >= 1


def test_pre_0323_missing_refs_schema_fails_clearly(tmp_path) -> None:
    service, path = _lineage_service(tmp_path)
    with duckdb.connect(str(path)) as con:
        con.execute("ALTER TABLE derived_metric_values DROP COLUMN selected_input_refs_json")
    with pytest.raises(ApiQueryError, match="selected lineage is unavailable"):
        service.issuer_content_range(
            schema_name="derived-metrics", cik=CIK_A,
            start=dt.date(2026, 1, 1), end=dt.date(2026, 9, 1),
            content_as_of=dt.datetime(2026, 9, 1), items=["capex_q"],
            fields=["period_end", "value"],
        )
    with pytest.raises(ValueError, match="selected lineage is unavailable"):
        issuer_derived_asof(CIK_A, dt.datetime(2026, 9, 1), path)


def test_aggregate_batch_limit_splits_without_raising_the_bound(tmp_path) -> None:
    _, path = _lineage_service(tmp_path)
    with duckdb.connect(str(path)) as con:
        hashes = registered_definition_hashes(con)
        accepted, diagnostics = qualify_issuer_derived_page(
            con,
            [{"derived_value_id": "root-owned", "period_end": dt.date(2026, 6, 30),
              "value": 20.0, "value_status": "valid"},
             {"derived_value_id": "root-owned-null", "period_end": dt.date(2026, 6, 30),
              "value": None, "value_status": "missing_input_or_domain"}],
            expected_cik=CIK_A, expected_definition_hashes=hashes, max_batch_nodes=2,
        )
    assert accepted == [True, True]
    assert diagnostics == []
