"""Upgrade an existing public catalog through the 0322 migration runner."""

from __future__ import annotations

import datetime as dt
import json

from atx_db.api.catalog import _record_schema_sha256, get_dataset
from atx_db.migrations import MIGRATIONS, apply_pending_migrations
from atx_db.migrations.bodies_0322 import _issuer_content_public_catalog
from atx_db.schema_contract import assert_schema_contract_version


def test_issuer_catalog_upgrade_from_predecessor_and_replay(tmp_store):
    con = tmp_store.con
    issuer = get_dataset("ATX.US.ISSUER_CONTENT")
    assert {schema.code for schema in issuer.schemas} == {
        "statements", "standardized", "ttm", "ratios", "shares", "derived-metrics"
    }
    assert any(migration.version == 322 for migration in MIGRATIONS)
    assert con.execute(
        "SELECT count(*) FROM schema_migrations WHERE version='0322'"
    ).fetchone()[0] == 1

    # The template is at head. Remove only 0322's publication and migration
    # record to reproduce the persisted 0321 predecessor without rebuilding it.
    con.execute("DELETE FROM schema_migrations WHERE version='0322'")
    for table in (
        "api_field_catalog", "api_schema_catalog", "api_dataset_catalog", "api_unit_price_catalog"
    ):
        con.execute(f"DELETE FROM {table} WHERE dataset_id=?", [issuer.code])
    con.execute(
        """
        UPDATE api_unit_price_catalog
        SET status='active',unit_price_per_gb=2.5
        WHERE dataset_id='ATX.US.FUNDAMENTALS' AND schema_code='reported'
          AND status='contract_required'
        """
    )
    assert con.execute(
        """
        SELECT count(*) FROM api_unit_price_catalog
        WHERE dataset_id='ATX.US.FUNDAMENTALS' AND schema_code='reported'
          AND status='active' AND unit_price_per_gb=2.5
        """
    ).fetchone()[0] == 1

    def rows(table, *, issuer_rows):
        predicate = "=" if issuer_rows else "<>"
        order = {
            "api_dataset_catalog": "dataset_id",
            "api_schema_catalog": "dataset_id,schema_code,schema_version",
            "api_field_catalog": "dataset_id,schema_code,schema_version,ordinal",
            "api_unit_price_catalog": "price_id",
        }[table]
        return con.execute(
            f"SELECT * FROM {table} WHERE dataset_id{predicate}? ORDER BY {order}",
            [issuer.code],
        ).fetchall()

    tables = (
        "api_dataset_catalog", "api_schema_catalog", "api_field_catalog", "api_unit_price_catalog"
    )
    previous_other_rows = {table: rows(table, issuer_rows=False) for table in tables}

    assert apply_pending_migrations(con) == [322]
    assert con.execute(
        "SELECT description,length(checksum) FROM schema_migrations WHERE version='0322'"
    ).fetchone() == ("issuer_content_public_catalog", 64)

    dataset_row = con.execute(
        """
        SELECT dataset_id,contract_version,title,description,asset_class,region,
               entitlement_code,default_schema,is_active
        FROM api_dataset_catalog WHERE dataset_id=?
        """,
        [issuer.code],
    ).fetchone()
    assert dataset_row == (
        issuer.code, issuer.version, issuer.title, issuer.description,
        issuer.asset_class, issuer.region, issuer.entitlement, issuer.default_schema, True,
    )
    schema_rows = con.execute(
        """
        SELECT schema_code,schema_version,title,description,source_table,time_field,
               natural_key_json,pit_policy,supported_encodings_json,max_sync_rows,
               schema_sha256,is_active
        FROM api_schema_catalog WHERE dataset_id=? ORDER BY schema_code
        """,
        [issuer.code],
    ).fetchall()
    assert schema_rows == sorted(
        (
            schema.code, schema.version, schema.title, schema.description,
            schema.source_table, schema.time_column,
            json.dumps(schema.natural_key, separators=(",", ":")),
            "available_at_lte_request_as_of_then_select_requested_revision_vintage",
            '["json","jsonl","csv","parquet","arrow"]',
            schema.max_sync_rows, _record_schema_sha256(schema), True,
        )
        for schema in issuer.schemas
    )
    field_rows = con.execute(
        """
        SELECT schema_code,schema_version,ordinal,field_name,source_column,
               data_type,semantic_type,unit,nullable,is_filterable,description
        FROM api_field_catalog WHERE dataset_id=? ORDER BY schema_code,ordinal
        """,
        [issuer.code],
    ).fetchall()
    assert field_rows == sorted(
        (
            schema.code, schema.version, ordinal, field.name, field.source_column,
            field.data_type,
            "identifier" if field.name.endswith("_id") else "observation",
            field.unit, field.nullable, field.filterable, field.description,
        )
        for schema in issuer.schemas
        for ordinal, field in enumerate(schema.fields, start=1)
    )
    price_rows = con.execute(
        """
        SELECT schema_code,mode,currency,billing_unit,unit_price_per_gb,status,valid_from
        FROM api_unit_price_catalog WHERE dataset_id=? ORDER BY schema_code
        """,
        [issuer.code],
    ).fetchall()
    assert price_rows == sorted(
        (
            schema.code, "historical", "USD", "uncompressed_arrow_bytes",
            None, "contract_required", dt.datetime(1900, 1, 1),
        )
        for schema in issuer.schemas
    )
    assert_schema_contract_version(con)
    assert {table: rows(table, issuer_rows=False) for table in tables} == previous_other_rows

    published_rows = {table: rows(table, issuer_rows=True) for table in tables}
    _issuer_content_public_catalog(con)
    assert {table: rows(table, issuer_rows=True) for table in tables} == published_rows
    assert {table: rows(table, issuer_rows=False) for table in tables} == previous_other_rows
    assert apply_pending_migrations(con) == []
    assert_schema_contract_version(con)
