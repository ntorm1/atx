"""The ordinary standardized API retains a later visible NULL conflict."""

from __future__ import annotations

import datetime as dt
import json
from pathlib import Path

from atx_db.api.catalog import _record_schema_sha256, get_schema, public_schema
from atx_db.api.models import BatchRangeRequest, RangeRequest
from atx_db.api.service import WarehouseReadService
from atx_db.migrations import apply_pending_migrations
from atx_db.migrations.bodies_0321 import _reported_eps_nullable_standardized_value
from atx_db.schema_contract import assert_schema_contract_version


def _captured_row(row):
    return {
        key: dt.datetime.fromisoformat(value)
        if key in ("source_loaded_at", "updated_at") else value
        for key, value in row.items()
    }


def _catalog_rows(conn, table, columns, *, order):
    cursor = conn.execute(
        f"SELECT {','.join(columns)} FROM {table} "
        "WHERE dataset_id='ATX.US.FUNDAMENTALS' AND schema_code='standardized' "
        "AND schema_version='2.0.0' "
        f"ORDER BY {order}"
    )
    return [dict(zip(columns, row, strict=True)) for row in cursor.fetchall()]


def test_existing_catalog_upgrade_and_public_standardized_null(tmp_store):
    conn = tmp_store.con
    schema = get_schema("ATX.US.FUNDAMENTALS", "standardized")
    fixture = json.loads((
        Path(__file__).parent / "data" / "standardized_2_0_0_0319_catalog.json"
    ).read_text(encoding="utf-8"))
    assert fixture["production_migration"] == 319
    assert len(fixture["schemas"]) == 1
    assert len(fixture["fields"]) == 33
    captured_schema = _captured_row(fixture["schemas"][0])
    captured_fields = [_captured_row(row) for row in fixture["fields"]]
    assert captured_schema["schema_sha256"] == (
        "36a994626594e7250cf558c4f2262e8a34ad7906e088e8a71ee3db5af7e0c985"
    )
    assert next(row for row in captured_fields if row["field_name"] == "value")["nullable"] is False
    assert schema.version == "3.0.0"
    assert public_schema(schema)["schema_sha256"] == _record_schema_sha256(schema)
    assert next(field for field in public_schema(schema)["fields"] if field["name"] == "value")["nullable"]

    # Install the exact source-observed 0319 public metadata, including its
    # original digest and provenance timestamps, over the current test template.
    for table, rows in (
        ("api_schema_catalog", [captured_schema]),
        ("api_field_catalog", captured_fields),
    ):
        columns = tuple(rows[0])
        placeholders = ",".join("?" for _ in columns)
        conn.executemany(
            f"INSERT INTO {table} ({','.join(columns)}) VALUES ({placeholders})",
            [tuple(row[column] for column in columns) for row in rows],
        )
    conn.execute(
        "DELETE FROM api_field_catalog WHERE dataset_id='ATX.US.FUNDAMENTALS' "
        "AND schema_code='standardized' AND schema_version='3.0.0'"
    )
    conn.execute(
        "DELETE FROM api_schema_catalog WHERE dataset_id='ATX.US.FUNDAMENTALS' "
        "AND schema_code='standardized' AND schema_version='3.0.0'"
    )
    conn.execute("ALTER TABLE fundamental_standardized ALTER COLUMN value SET NOT NULL")
    conn.execute(
        "UPDATE field_catalog SET nullable=false, description='Standardized value.' "
        "WHERE table_name='fundamental_standardized' AND field_name='value'"
    )
    assert conn.execute(
        "SELECT count(*) FROM schema_migrations WHERE version='0321'"
    ).fetchone() == (1,)
    conn.execute("DELETE FROM schema_migrations WHERE version='0321'")
    untouched = {
        table: conn.execute(
            f"SELECT * FROM {table} WHERE "
            + ("dataset_id <> 'ATX.US.FUNDAMENTALS'" if table != "api_unit_price_catalog"
               else "true")
            + " ORDER BY ALL"
        ).fetchall()
        for table in ("api_schema_catalog", "api_field_catalog", "api_unit_price_catalog")
    }
    schema_columns = tuple(captured_schema)
    field_columns = tuple(captured_fields[0])
    assert _catalog_rows(
        conn, "api_schema_catalog", schema_columns, order="schema_version"
    ) == [captured_schema]
    assert _catalog_rows(conn, "api_field_catalog", field_columns, order="ordinal") == captured_fields

    assert apply_pending_migrations(conn) == [321]
    assert conn.execute(
        "SELECT description,length(checksum) FROM schema_migrations WHERE version='0321'"
    ).fetchone() == ("reported_eps_nullable_standardized_value", 64)
    assert_schema_contract_version(conn)
    old_schema_after = _catalog_rows(conn, "api_schema_catalog", schema_columns, order="schema_version")[0]
    assert old_schema_after["is_active"] is False
    assert old_schema_after["updated_at"] >= captured_schema["updated_at"]
    assert {key: value for key, value in old_schema_after.items()
            if key not in ("is_active", "updated_at")} == {
        key: value for key, value in captured_schema.items()
        if key not in ("is_active", "updated_at")
    }
    assert _catalog_rows(conn, "api_field_catalog", field_columns, order="ordinal") == captured_fields
    assert conn.execute(
        "SELECT schema_sha256,is_active FROM api_schema_catalog "
        "WHERE dataset_id='ATX.US.FUNDAMENTALS' AND schema_code='standardized' "
        "AND schema_version='3.0.0'"
    ).fetchone() == (_record_schema_sha256(schema), True)
    assert conn.execute(
        "SELECT nullable FROM api_field_catalog WHERE dataset_id='ATX.US.FUNDAMENTALS' "
        "AND schema_code='standardized' AND schema_version='3.0.0' AND field_name='value'"
    ).fetchone() == (True,)
    assert {
        table: conn.execute(
            f"SELECT * FROM {table} WHERE "
            + ("dataset_id <> 'ATX.US.FUNDAMENTALS'" if table != "api_unit_price_catalog"
               else "true")
            + " ORDER BY ALL"
        ).fetchall()
        for table in untouched
    } == untouched

    # A direct replay cannot rewrite current or historical API metadata,
    # catalog timestamps, or prices. The runner skips the recorded migration.
    replay_tables = (
        ("api_dataset_catalog", "dataset_id='ATX.US.FUNDAMENTALS'", "dataset_id"),
        ("api_schema_catalog", "true", "dataset_id,schema_code,schema_version"),
        ("api_field_catalog", "true", "dataset_id,schema_code,schema_version,ordinal"),
        ("api_unit_price_catalog", "true", "price_id"),
        ("table_catalog", "table_name='fundamental_standardized'", "table_name"),
        ("field_catalog", "table_name='fundamental_standardized'", "field_name"),
    )
    replay_snapshot = {
        table: conn.execute(f"SELECT * FROM {table} WHERE {predicate} ORDER BY {order}").fetchall()
        for table, predicate, order in replay_tables
    }
    _reported_eps_nullable_standardized_value(conn)
    assert {
        table: conn.execute(f"SELECT * FROM {table} WHERE {predicate} ORDER BY {order}").fetchall()
        for table, predicate, order in replay_tables
    } == replay_snapshot
    assert apply_pending_migrations(conn) == []
    assert_schema_contract_version(conn)

    # A later conflict must win the PIT revision ranking even with NULL value.
    conn.execute(
        "INSERT INTO securities (security_id,primary_symbol,name,source) "
        "VALUES ('owner-eps','EPS','EPS','fixture')"
    )
    conn.executemany(
        """
        INSERT INTO fundamental_standardized (
            standardized_id,source,security_id,symbol,cik,item_id,canonical_code,
            basis,period_start,period_end,value,as_of_date,available_at,
            input_codes_json,input_item_ids_json,rule_id,combination_rule,
            source_loaded_at
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        """,
        [
            ("eps-release", "fixture", "owner-eps", "EPS", "0000000001", 1,
             "eps_diluted", "quarterly", dt.date(2025, 1, 1), dt.date(2025, 3, 31),
             1.5, dt.date(2025, 3, 31), dt.datetime(2025, 5, 3, 22),
             '["release"]', "[1]", "fixture", "single", dt.datetime(2025, 5, 3, 22)),
            ("eps-conflict", "fixture", "owner-eps", "EPS", "0000000001", 1,
             "eps_diluted", "quarterly", dt.date(2025, 1, 1), dt.date(2025, 3, 31),
             None, dt.date(2025, 3, 31), dt.datetime(2025, 5, 20, 22),
             '["conflicting_direct_eps"]', "[1]", "fixture", "single",
             dt.datetime(2025, 5, 20, 22)),
        ],
    )
    conn.execute("CHECKPOINT")
    tmp_store.connection.close()
    tmp_store.connection = None
    service = WarehouseReadService(tmp_store.path)
    request = {
        "dataset": "ATX.US.FUNDAMENTALS", "schema": "standardized",
        "symbols": ["owner-eps"], "stype_in": "security_id",
        "start": "2025-03-31", "end": "2025-04-01",
        "items": ["eps_diluted"], "basis": ["quarterly"],
        "fields": ["security_id", "item", "value", "available_at"],
    }
    early = service.get_range(RangeRequest.model_validate({
        **request, "as_of": "2025-05-10T00:00:00Z",
    }))
    late = service.get_range(RangeRequest.model_validate({
        **request, "as_of": "2025-05-21T00:00:00Z",
    }))
    assert [row["value"] for row in early.data] == [1.5]
    assert [row["value"] for row in late.data] == [None]
    assert late.metadata["schema_version"] == "3.0.0"

    with service.stream_range(BatchRangeRequest.model_validate({
        **request, "as_of": "2025-05-21T00:00:00Z",
    })) as stream:
        batches = list(stream.batches())
    assert len(batches) == 1
    assert batches[0].schema.field("value").nullable is True
    assert batches[0].to_pylist()[0]["value"] is None
