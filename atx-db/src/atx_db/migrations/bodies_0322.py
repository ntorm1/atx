"""Publish issuer-content API metadata in warehouses already past the API seed."""

from __future__ import annotations

import datetime as dt
import hashlib
import json

import duckdb

from ..api.catalog import _record_schema_sha256, get_dataset
from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _issuer_content_public_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    issuer = get_dataset("ATX.US.ISSUER_CONTENT")
    dataset_row = (
        issuer.code, issuer.version, issuer.title, issuer.description,
        issuer.asset_class, issuer.region, issuer.entitlement, issuer.default_schema, True,
    )
    conn.execute(
        """
        INSERT OR IGNORE INTO api_dataset_catalog (
            dataset_id,contract_version,title,description,asset_class,region,
            entitlement_code,default_schema,is_active,source_loaded_at,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,now(),now())
        """,
        dataset_row,
    )

    schema_rows = [
        (
            issuer.code, schema.code, schema.version, schema.title, schema.description,
            schema.source_table, schema.time_column,
            json.dumps(schema.natural_key, separators=(",", ":")),
            "available_at_lte_request_as_of_then_select_requested_revision_vintage",
            '["json","jsonl","csv","parquet","arrow"]',
            schema.max_sync_rows, _record_schema_sha256(schema), True,
        )
        for schema in issuer.schemas
    ]
    conn.executemany(
        """
        INSERT OR IGNORE INTO api_schema_catalog (
            dataset_id,schema_code,schema_version,title,description,source_table,
            time_field,natural_key_json,pit_policy,supported_encodings_json,
            max_sync_rows,schema_sha256,is_active,source_loaded_at,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,now(),now())
        """,
        schema_rows,
    )

    field_rows = [
        (
            issuer.code, schema.code, schema.version, ordinal, field.name,
            field.source_column, field.data_type,
            "identifier" if field.name.endswith("_id") else "observation",
            field.unit, field.nullable, field.filterable, field.description,
        )
        for schema in issuer.schemas
        for ordinal, field in enumerate(schema.fields, start=1)
    ]
    conn.executemany(
        """
        INSERT OR IGNORE INTO api_field_catalog (
            dataset_id,schema_code,schema_version,ordinal,field_name,source_column,
            data_type,semantic_type,unit,nullable,is_filterable,description,
            source_loaded_at,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,now(),now())
        """,
        field_rows,
    )

    # Existing rows are immutable here. Reject a conflicting partial publication
    # instead of silently replacing API metadata or its provenance timestamps.
    actual_dataset = conn.execute(
        """
        SELECT dataset_id,contract_version,title,description,asset_class,region,
               entitlement_code,default_schema,is_active
        FROM api_dataset_catalog WHERE dataset_id=?
        """,
        [issuer.code],
    ).fetchone()
    actual_schemas = conn.execute(
        """
        SELECT dataset_id,schema_code,schema_version,title,description,source_table,
               time_field,natural_key_json,pit_policy,supported_encodings_json,
               max_sync_rows,schema_sha256,is_active
        FROM api_schema_catalog WHERE dataset_id=? ORDER BY schema_code,schema_version
        """,
        [issuer.code],
    ).fetchall()
    actual_fields = conn.execute(
        """
        SELECT dataset_id,schema_code,schema_version,ordinal,field_name,source_column,
               data_type,semantic_type,unit,nullable,is_filterable,description
        FROM api_field_catalog WHERE dataset_id=? ORDER BY schema_code,schema_version,ordinal
        """,
        [issuer.code],
    ).fetchall()
    if (
        actual_dataset != dataset_row
        or actual_schemas != sorted(schema_rows, key=lambda row: (row[1], row[2]))
        or actual_fields != sorted(field_rows, key=lambda row: (row[1], row[2], row[3]))
    ):
        raise RuntimeError("issuer-content API catalog conflicts with the 0322 contract")

    # Match 0270's unpriced historical baseline. The deterministic ID and
    # INSERT OR IGNORE preserve any existing price history or active price.
    valid_from = dt.datetime(1900, 1, 1)
    conn.executemany(
        """
        INSERT OR IGNORE INTO api_unit_price_catalog (
            price_id,dataset_id,schema_code,mode,currency,billing_unit,
            unit_price_per_gb,status,valid_from
        ) VALUES (?,?,?,?,?,?,NULL,'contract_required',?)
        """,
        [
            (
                hashlib.sha256(
                    f"{issuer.code}|{schema.code}|historical|USD|{valid_from.isoformat()}".encode()
                ).hexdigest(),
                issuer.code, schema.code, "historical", "USD",
                "uncompressed_arrow_bytes", valid_from,
            )
            for schema in issuer.schemas
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=322, name="issuer_content_public_catalog", up=_issuer_content_public_catalog)
]
