from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from db.factor_panel import (
    PANEL_EXPORT_GATE_CHECK_NAME,
    assemble_factor_panel_long,
    assert_factor_panel_export_ready,
    describe_factor_panel,
    export_factor_panel,
    factor_panel_export_gate_report,
    main,
    pivot_factor_panel_wide,
    read_panel_asof,
)
from db.lake import DEFAULT_EXPORT_OBJECTS, validate_lake_export


def _factor_row(
    surface: str,
    security_id: str,
    factor_id: str,
    value: float,
    *,
    as_of_date: str,
    available_at: str,
    source_loaded_at: str = "2024-02-01 22:00:00",
) -> dict[str, object]:
    row = {
        "factor_value_id": f"{surface}-{security_id}-{factor_id}",
        "factor_id": factor_id,
        "factor_name": factor_id.replace("_", " ").title(),
        "family": "fixture_family",
        "security_id": security_id,
        "symbol": security_id.replace("SEC-", ""),
        "as_of_date": pd.Timestamp(as_of_date).date(),
        "raw_value": value,
        "value": value,
        "available_at": pd.Timestamp(available_at),
        "input_ids_json": "[]",
        "input_lineage_json": json.dumps([{"surface": surface, "available_at": available_at}]),
        "is_latest_revision": True,
        "run_id": "panel-fixture",
        "source": "pytest",
        "source_loaded_at": pd.Timestamp(source_loaded_at),
    }
    if surface == "cross":
        row["domain"] = "price_liquidity"
        row["source_row_id"] = f"source-{security_id}-{factor_id}"
    return row


def _membership(
    security_id: str,
    *,
    is_member: bool = True,
    valid_from: str = "2024-01-01",
    valid_to: str | None = None,
    available_at: str = "2024-01-01 09:30:00",
) -> dict[str, object]:
    return {
        "universe_id": "us_common_equity_liquid_v1",
        "security_id": security_id,
        "symbol": security_id.replace("SEC-", ""),
        "valid_from": pd.Timestamp(valid_from).date(),
        "valid_to": pd.Timestamp(valid_to).date() if valid_to else None,
        "as_of_date": pd.Timestamp(valid_from).date(),
        "is_member": is_member,
        "reason": "member" if is_member else "liquidity_screen_fail",
        "rules_json": "{}",
        "decision_count": 1,
        "available_at": pd.Timestamp(available_at),
        "source": "pytest",
        "run_id": "universe-fixture",
        "is_latest_revision": True,
        "source_loaded_at": pd.Timestamp("2024-01-01 09:31:00"),
    }


def _insert_factor_value_fixtures(tmp_store) -> None:
    fundamental = _factor_row(
        "fundamental",
        "SEC-A",
        "profitability_gross_profitability",
        0.8,
        as_of_date="2023-12-31",
        available_at="2024-02-01 10:00:00",
    )
    cross = _factor_row(
        "cross",
        "SEC-A",
        "price_momentum_21d",
        1.0,
        as_of_date="2024-02-01",
        available_at="2024-02-01 21:00:00",
    )
    non_member = _factor_row(
        "cross",
        "SEC-B",
        "price_momentum_21d",
        0.2,
        as_of_date="2024-02-01",
        available_at="2024-02-01 21:00:00",
    )
    future_visible = _factor_row(
        "cross",
        "SEC-C",
        "price_momentum_21d",
        0.5,
        as_of_date="2024-02-01",
        available_at="2024-02-02 09:00:00",
    )

    tmp_store.con.execute(
        """
        INSERT INTO fundamental_factor_values (
            factor_value_id, factor_id, factor_name, family, security_id, symbol,
            as_of_date, raw_value, value, available_at, input_ids_json,
            input_lineage_json, is_latest_revision, run_id, source, source_loaded_at
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        [
            fundamental["factor_value_id"],
            fundamental["factor_id"],
            fundamental["factor_name"],
            fundamental["family"],
            fundamental["security_id"],
            fundamental["symbol"],
            fundamental["as_of_date"],
            fundamental["raw_value"],
            fundamental["value"],
            fundamental["available_at"],
            fundamental["input_ids_json"],
            fundamental["input_lineage_json"],
            fundamental["is_latest_revision"],
            fundamental["run_id"],
            fundamental["source"],
            fundamental["source_loaded_at"],
        ],
    )
    for row in (cross, non_member, future_visible):
        tmp_store.con.execute(
            """
            INSERT INTO cross_domain_factor_values (
                factor_value_id, factor_id, factor_name, domain, family, security_id, symbol,
                as_of_date, raw_value, value, available_at, source_row_id, input_ids_json,
                input_lineage_json, is_latest_revision, run_id, source, source_loaded_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                row["factor_value_id"],
                row["factor_id"],
                row["factor_name"],
                row["domain"],
                row["family"],
                row["security_id"],
                row["symbol"],
                row["as_of_date"],
                row["raw_value"],
                row["value"],
                row["available_at"],
                row["source_row_id"],
                row["input_ids_json"],
                row["input_lineage_json"],
                row["is_latest_revision"],
                row["run_id"],
                row["source"],
                row["source_loaded_at"],
            ],
        )
    for row in (_membership("SEC-A"), _membership("SEC-B", is_member=False), _membership("SEC-C")):
        tmp_store.con.execute(
            """
            INSERT INTO universe_membership (
                universe_id, security_id, symbol, valid_from, valid_to, as_of_date,
                is_member, reason, rules_json, decision_count, available_at, source,
                run_id, is_latest_revision, source_loaded_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                row["universe_id"],
                row["security_id"],
                row["symbol"],
                row["valid_from"],
                row["valid_to"],
                row["as_of_date"],
                row["is_member"],
                row["reason"],
                row["rules_json"],
                row["decision_count"],
                row["available_at"],
                row["source"],
                row["run_id"],
                row["is_latest_revision"],
                row["source_loaded_at"],
            ],
        )


def test_assemble_factor_panel_long_uses_decision_dates_and_universe_membership() -> None:
    fundamental = pd.DataFrame(
        [
            _factor_row(
                "fundamental",
                "SEC-A",
                "profitability_gross_profitability",
                0.8,
                as_of_date="2023-12-31",
                available_at="2024-02-01 10:00:00",
            ),
            _factor_row(
                "fundamental",
                "SEC-B",
                "profitability_gross_profitability",
                0.1,
                as_of_date="2023-12-31",
                available_at="2024-02-01 10:00:00",
            ),
        ]
    )
    cross_domain = pd.DataFrame(
        [
            _factor_row(
                "cross",
                "SEC-A",
                "price_momentum_21d",
                1.0,
                as_of_date="2024-02-01",
                available_at="2024-02-01 21:00:00",
            ),
            _factor_row(
                "cross",
                "SEC-C",
                "price_momentum_21d",
                0.5,
                as_of_date="2024-02-01",
                available_at="2024-02-02 09:00:00",
            ),
        ]
    )
    membership = pd.DataFrame(
        [
            _membership("SEC-A"),
            _membership("SEC-B", is_member=False),
            _membership("SEC-C"),
        ]
    )

    panel = assemble_factor_panel_long(
        fundamental,
        cross_domain,
        universe_membership=membership,
        as_of_date=dt.date(2024, 2, 1),
    )

    assert list(panel["security_id"].unique()) == ["SEC-A"]
    assert set(panel["factor_id"]) == {"profitability_gross_profitability", "price_momentum_21d"}
    assert set(panel["as_of_date"]) == {dt.date(2024, 2, 1)}
    assert all(pd.to_datetime(panel["available_at"]).dt.date <= panel["as_of_date"])

    wide = pivot_factor_panel_wide(panel)
    assert list(wide.columns) == [
        "security_id",
        "as_of_date",
        "price_momentum_21d",
        "profitability_gross_profitability",
    ]
    assert wide.loc[0, "price_momentum_21d"] == 1.0
    assert wide.loc[0, "profitability_gross_profitability"] == 0.8


def test_panel_dedup_is_deterministic_on_availability_ties() -> None:
    row_low = _factor_row(
        "fundamental",
        "SEC-A",
        "profitability_gross_profitability",
        0.8,
        as_of_date="2024-02-01",
        available_at="2024-02-01 10:00:00",
        source_loaded_at="2024-02-01 12:00:00",
    )
    row_low["run_id"] = "run-aaaa"
    row_low["factor_value_id"] = "fundamental-SEC-A-tie-a"

    row_high = _factor_row(
        "fundamental",
        "SEC-A",
        "profitability_gross_profitability",
        0.5,
        as_of_date="2024-02-01",
        available_at="2024-02-01 10:00:00",
        source_loaded_at="2024-02-01 12:00:00",
    )
    row_high["run_id"] = "run-bbbb"
    row_high["factor_value_id"] = "fundamental-SEC-A-tie-b"

    forward = pd.DataFrame([row_low, row_high])
    shuffled = pd.DataFrame([row_high, row_low])

    panel_forward = assemble_factor_panel_long(forward)
    panel_shuffled = assemble_factor_panel_long(shuffled)

    assert len(panel_forward) == 1
    assert len(panel_shuffled) == 1
    assert panel_forward.loc[0, "run_id"] == panel_shuffled.loc[0, "run_id"]
    assert panel_forward.loc[0, "value"] == panel_shuffled.loc[0, "value"]
    assert panel_forward.loc[0, "run_id"] == "run-bbbb"


def test_factor_panel_views_resolve_long_and_wide_with_pit_universe_filter(tmp_store) -> None:
    _insert_factor_value_fixtures(tmp_store)

    long_rows = tmp_store.con.execute(
        """
        SELECT security_id, as_of_date, factor_id, value, CAST(available_at AS DATE) AS available_date
        FROM v_factor_panel
        WHERE as_of_date = DATE '2024-02-01'
        ORDER BY security_id, factor_id
        """
    ).fetchall()

    assert long_rows == [
        ("SEC-A", dt.date(2024, 2, 1), "price_momentum_21d", 1.0, dt.date(2024, 2, 1)),
        ("SEC-A", dt.date(2024, 2, 1), "profitability_gross_profitability", 0.8, dt.date(2024, 2, 1)),
    ]
    all_rows = tmp_store.con.execute(
        """
        SELECT security_id, as_of_date, factor_id, CAST(available_at AS DATE) AS available_date
        FROM v_factor_panel
        ORDER BY security_id, as_of_date, factor_id
        """
    ).fetchall()
    assert ("SEC-C", dt.date(2024, 2, 2), "price_momentum_21d", dt.date(2024, 2, 2)) in all_rows
    assert all(available_date <= as_of_date for _, as_of_date, _, available_date in all_rows)
    assert not any(row[0] == "SEC-B" for row in all_rows)

    wide_row = tmp_store.con.execute(
        """
        SELECT security_id, as_of_date, factor_values_json, factor_count
        FROM v_factor_panel_wide
        WHERE security_id = 'SEC-A'
          AND as_of_date = DATE '2024-02-01'
        """
    ).fetchone()
    assert wide_row[0:2] == ("SEC-A", dt.date(2024, 2, 1))
    assert json.loads(wide_row[2]) == {
        "price_momentum_21d": 1.0,
        "profitability_gross_profitability": 0.8,
    }
    assert wide_row[3] == 2

    catalog_counts = tmp_store.con.execute(
        """
        SELECT
            (SELECT count(*) FROM dataset_catalog WHERE dataset_id = 'factor_panel'),
            (SELECT count(*) FROM table_catalog WHERE table_name = 'v_factor_panel'),
            (SELECT count(*) FROM table_catalog WHERE table_name = 'v_factor_panel_wide'),
            (SELECT count(*) FROM quality_check_registry WHERE check_name = 'factor_panel_export_contract')
        """
    ).fetchone()
    assert catalog_counts == (1, 1, 1, 1)


def test_factor_panel_exports_partitioned_lake_object_with_schema_contract(tmp_store, tmp_path) -> None:
    from db.connection import DuckDBStore

    _insert_factor_value_fixtures(tmp_store)
    db_path = tmp_store.path
    tmp_store.con.execute("CHECKPOINT")
    tmp_store.connection.close()
    tmp_store.connection = None

    result = export_factor_panel(db_path, lake_root=tmp_path / "lake", incremental=True)
    manifest = json.loads(result.manifest_path.read_text(encoding="utf-8"))

    assert "v_factor_panel" in DEFAULT_EXPORT_OBJECTS
    assert result.object_name == "v_factor_panel"
    assert result.rows == 3
    assert manifest["partition_columns"] == ["as_of_date"]
    assert manifest["watermark_column"] == "available_at"
    assert manifest["schema_sha256"] == manifest["expected_schema_sha256"]
    assert len(manifest["files"]) == 2
    assert {item["partition_values"]["as_of_date"] for item in manifest["files"]} == {
        "2024-02-01",
        "2024-02-02",
    }
    assert all(item["path"].endswith("part-00000.parquet") for item in manifest["files"])

    summary = validate_lake_export(db_path, export_run_id=result.export_run_id)
    assert summary.problems == []
    assert summary.rows_checked == 3
    assert summary.files_readable == 2

    with DuckDBStore(db_path, read_only=True) as store:
        contract = store.con.execute(
            """
            SELECT expected_schema_sha256, partition_columns_json, watermark_column
            FROM lake_export_object_contract
            WHERE object_name = 'v_factor_panel'
            """
        ).fetchone()
        audit = store.con.execute(
            """
            SELECT schema_sha256, expected_schema_sha256
            FROM lake_export_files
            WHERE export_run_id = ?
              AND object_name = 'v_factor_panel'
            """,
            [result.export_run_id],
        ).fetchone()

    assert contract == (manifest["schema_sha256"], '["as_of_date"]', "available_at")
    assert audit == (manifest["schema_sha256"], manifest["schema_sha256"])

    with DuckDBStore(db_path) as store:
        store.con.execute(
            """
            UPDATE lake_export_object_contract
            SET expected_schema_sha256 = repeat('0', 64)
            WHERE object_name = 'v_factor_panel'
            """
        )

    with pytest.raises(ValueError, match="Schema SHA-256 mismatch for v_factor_panel"):
        export_factor_panel(db_path, lake_root=tmp_path / "lake_bad", incremental=True)


def test_factor_panel_export_gate_blocks_contract_lookahead_and_non_member_rows(tmp_store, tmp_path) -> None:
    from db.quality import run_warehouse_quality_checks

    _insert_factor_value_fixtures(tmp_store)

    clean_report = factor_panel_export_gate_report(tmp_store)
    assert clean_report["violation_count"] == 0.0
    assert_factor_panel_export_ready(tmp_store)
    clean_result = run_warehouse_quality_checks(
        tmp_store,
        record=False,
        check_names=(PANEL_EXPORT_GATE_CHECK_NAME,),
    )
    assert len(clean_result) == 1
    assert clean_result[0].status == "passed"
    assert clean_result[0].severity == "critical"

    tmp_store.con.execute(
        """
        CREATE OR REPLACE VIEW v_factor_panel AS
        SELECT
            'SEC-BAD'::VARCHAR AS security_id,
            DATE '2024-02-01' AS as_of_date,
            'bad_factor'::VARCHAR AS factor_id,
            1.0::DOUBLE AS value,
            TIMESTAMP '2024-02-02 09:00:00' AS available_at,
            TIMESTAMP '2024-02-02 09:01:00' AS source_loaded_at,
            'bad-run'::VARCHAR AS run_id,
            '[{"available_at":"2024-02-03T00:00:00"}]'::VARCHAR AS input_lineage_json,
            'undeclared'::VARCHAR AS extra_export_column
        """
    )
    tmp_store.con.execute(
        """
        UPDATE panel_contract
        SET unit = 'wrong_unit'
        WHERE column_name = 'value'
        """
    )

    bad_report = factor_panel_export_gate_report(tmp_store)
    kinds = {item["kind"] for item in bad_report["violations"]}
    assert bad_report["violation_count"] >= 5.0
    assert {
        "panel_column_shape_mismatch",
        "panel_contract_metadata_mismatch",
        "panel_available_at_lookahead",
        "panel_input_lineage_lookahead",
        "panel_universe_membership_violation",
    } <= kinds

    with pytest.raises(ValueError, match="factor panel export gate failed"):
        assert_factor_panel_export_ready(tmp_store)

    bad_result = run_warehouse_quality_checks(
        tmp_store,
        record=False,
        check_names=(PANEL_EXPORT_GATE_CHECK_NAME,),
    )
    assert len(bad_result) == 1
    assert bad_result[0].status == "failed"
    assert bad_result[0].severity == "critical"
    assert bad_result[0].observed_value == bad_report["violation_count"]

    db_path = tmp_store.path
    tmp_store.con.execute("CHECKPOINT")
    tmp_store.connection.close()
    tmp_store.connection = None

    with pytest.raises(ValueError, match="factor panel export gate failed"):
        export_factor_panel(db_path, lake_root=tmp_path / "blocked_lake", incremental=True)


def test_read_panel_asof_catalog_and_cli_round_trip(tmp_store, tmp_path, capsys) -> None:
    _insert_factor_value_fixtures(tmp_store)

    early = read_panel_asof(dt.date(2024, 2, 1), store=tmp_store)
    assert set(early["security_id"]) == {"SEC-A"}
    assert set(early["factor_id"]) == {"price_momentum_21d", "profitability_gross_profitability"}
    assert set(early["as_of_date"]) == {pd.Timestamp("2024-02-01")}

    late = read_panel_asof(
        dt.date(2024, 2, 3),
        store=tmp_store,
        factor_ids=("price_momentum_21d",),
    )
    assert set(late["security_id"]) == {"SEC-A", "SEC-C"}
    assert set(late["factor_id"]) == {"price_momentum_21d"}
    assert set(late["as_of_date"]) == {pd.Timestamp("2024-02-03")}

    wide = read_panel_asof(
        dt.date(2024, 2, 3),
        store=tmp_store,
        factor_ids=("price_momentum_21d",),
        wide=True,
    )
    assert list(wide.columns) == ["security_id", "as_of_date", "price_momentum_21d"]
    assert set(wide["security_id"]) == {"SEC-A", "SEC-C"}

    description = describe_factor_panel(store=tmp_store)
    assert description["row_count"] == 3
    assert description["factor_count"] == 2
    assert description["partition_columns"] == ["as_of_date"]
    assert description["watermark_column"] == "available_at"

    index_names = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT index_name
            FROM duckdb_indexes()
            WHERE index_name LIKE 'idx_%_panel_read'
            """
        ).fetchall()
    }
    assert {
        "idx_fundamental_factor_values_panel_read",
        "idx_cross_domain_factor_values_panel_read",
        "idx_universe_membership_panel_read",
    } <= index_names
    wide_catalog_count = tmp_store.con.execute(
        "SELECT count(*) FROM dataset_catalog WHERE dataset_id = 'factor_panel_wide'"
    ).fetchone()[0]
    assert wide_catalog_count == 1

    db_path = tmp_store.path
    tmp_store.con.execute("CHECKPOINT")
    tmp_store.connection.close()
    tmp_store.connection = None

    assert main(["--db-path", str(db_path), "describe"]) == 0
    describe_payload = json.loads(capsys.readouterr().out)
    assert describe_payload["row_count"] == 3
    assert describe_payload["partition_columns"] == ["as_of_date"]

    assert main(
        [
            "--db-path",
            str(db_path),
            "read",
            "--as-of",
            "2024-02-03",
            "--wide",
            "--factor-id",
            "price_momentum_21d",
        ]
    ) == 0
    read_output = capsys.readouterr().out
    assert "price_momentum_21d" in read_output
    assert "SEC-C" in read_output

    assert main(["--db-path", str(db_path), "export", "--lake-root", str(tmp_path / "cli_lake")]) == 0
    export_payload = json.loads(capsys.readouterr().out)
    assert export_payload["object_name"] == "v_factor_panel"
    assert export_payload["rows"] == 3


def test_panel_read_paths_open_read_only(tmp_store, tmp_path, monkeypatch) -> None:
    """S3-8: read_panel_asof/describe_factor_panel/export_factor_panel must not open the
    warehouse writable when driving off a db_path (writer-lock hazard for concurrent reads).
    """
    import contextlib

    from db import factor_panel as factor_panel_module

    _insert_factor_value_fixtures(tmp_store)

    db_path = tmp_store.path
    tmp_store.con.execute("CHECKPOINT")
    tmp_store.connection.close()
    tmp_store.connection = None

    captured_read_only: list[bool] = []
    real_connect = factor_panel_module.connect

    @contextlib.contextmanager
    def _capturing_connect(path, *, read_only=False):
        captured_read_only.append(read_only)
        with real_connect(path, read_only=read_only) as store:
            yield store

    monkeypatch.setattr(factor_panel_module, "connect", _capturing_connect)

    read_panel_asof(dt.date(2024, 2, 1), db_path=db_path)
    describe_factor_panel(db_path=db_path)
    export_factor_panel(db_path, lake_root=tmp_path / "read_only_lake")

    assert len(captured_read_only) == 3
    assert captured_read_only == [True, True, True]
