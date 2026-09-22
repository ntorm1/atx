"""Reported-quarter EPS bridge schema support."""

from __future__ import annotations

import json

import duckdb

from ..api.catalog import _record_schema_sha256, get_schema
from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _publish_nullable_standardized_contract(conn: duckdb.DuckDBPyConnection) -> None:
    """Add only the revised public standardized schema to an existing API catalog."""

    schema = get_schema("ATX.US.FUNDAMENTALS", "standardized")
    if schema.version != "3.0.0" or schema.field("value").nullable is not True:
        raise RuntimeError("0321 requires the nullable standardized 3.0.0 API contract")
    schema_row = (
        schema.dataset, schema.code, schema.version, schema.title, schema.description,
        schema.source_table, schema.time_column,
        json.dumps(schema.natural_key, separators=(",", ":")),
        "available_at_lte_request_as_of_then_select_requested_revision_vintage",
        '["json","jsonl","csv","parquet","arrow"]', schema.max_sync_rows,
        _record_schema_sha256(schema), True,
    )
    conn.execute(
        """
        INSERT OR IGNORE INTO api_schema_catalog (
            dataset_id,schema_code,schema_version,title,description,source_table,
            time_field,natural_key_json,pit_policy,supported_encodings_json,
            max_sync_rows,schema_sha256,is_active,source_loaded_at,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,now(),now())
        """,
        schema_row,
    )
    field_rows = [
        (
            schema.dataset, schema.code, schema.version, ordinal, field.name,
            field.source_column, field.data_type,
            "identifier" if field.name.endswith("_id") else "observation",
            field.unit, field.nullable, field.filterable, field.description,
        )
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
    actual_schema = conn.execute(
        """
        SELECT dataset_id,schema_code,schema_version,title,description,source_table,
               time_field,natural_key_json,pit_policy,supported_encodings_json,
               max_sync_rows,schema_sha256,is_active
        FROM api_schema_catalog
        WHERE dataset_id=? AND schema_code=? AND schema_version=?
        """,
        [schema.dataset, schema.code, schema.version],
    ).fetchone()
    actual_fields = conn.execute(
        """
        SELECT dataset_id,schema_code,schema_version,ordinal,field_name,source_column,
               data_type,semantic_type,unit,nullable,is_filterable,description
        FROM api_field_catalog
        WHERE dataset_id=? AND schema_code=? AND schema_version=?
        ORDER BY ordinal
        """,
        [schema.dataset, schema.code, schema.version],
    ).fetchall()
    if actual_schema != schema_row or actual_fields != field_rows:
        raise RuntimeError("nullable standardized API catalog conflicts with the 0321 contract")

    # Keep the published 2.0.0 row and hash as history. The unversioned API
    # resolves the current 3.0.0 schema, so its predecessor is no longer active.
    conn.execute(
        """
        UPDATE api_schema_catalog SET is_active=false, updated_at=now()
        WHERE dataset_id=? AND schema_code=? AND schema_version='2.0.0'
          AND is_active=true
        """,
        [schema.dataset, schema.code],
    )


def _reported_eps_nullable_standardized_value(conn: duckdb.DuckDBPyConnection) -> None:
    """Represent a later, visible reported-EPS conflict as a PIT NULL state.

    No raw Company Facts or release row is changed.  ``value`` is nullable only
    because an explicitly conflicting direct EPS must replace an earlier valid
    state without fabricating a winner.  The bridge writes the reason and both
    source identities through the existing input-lineage fields and the
    standardization-exception relation.
    """

    conn.execute("ALTER TABLE fundamental_standardized ALTER COLUMN value DROP NOT NULL")
    conn.execute(
        """
        UPDATE table_catalog
        SET description = concat_ws(' ', nullif(description, ''), ?),
            pit_notes = concat_ws(' ', nullif(pit_notes, ''), ?),
            updated_at = now()
        WHERE table_name = 'fundamental_standardized'
          AND strpos(coalesce(pit_notes, ''), 'reported_eps_conflict_v1') = 0
        """,
        [
            'A NULL value can be an explicit unavailable standardized state when later visible source evidence conflicts.',
            'reported_eps_conflict_v1: rank the complete latest state before filtering usable values; a NULL conflict must not revive an earlier EPS value.',
        ],
    )
    conn.execute(
        """
        UPDATE field_catalog
        SET nullable = true,
            description = concat_ws(' ', nullif(description, ''), ?),
            updated_at = now()
        WHERE table_name = 'fundamental_standardized'
          AND field_name = 'value'
          AND strpos(coalesce(description, ''), 'reported_eps_conflict_v1') = 0
        """,
        [
            'reported_eps_conflict_v1: NULL may be an explicit later unavailable state; inspect input_codes_json and standardization exceptions before treating it as absent source data.',
        ],
    )
    _publish_nullable_standardized_contract(conn)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=321,
        name='reported_eps_nullable_standardized_value',
        up=_reported_eps_nullable_standardized_value,
    )
]
