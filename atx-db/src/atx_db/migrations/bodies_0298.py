"""Public restatement-event surface over standardized revision chains."""

from __future__ import annotations

import duckdb

from ..api.catalog import DATASETS, _record_schema_sha256
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin
from .bodies_0267 import _seed_public_contract


def _restatement_event_surface(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE OR REPLACE VIEW v_fundamental_restatement_events AS
        WITH chains AS (
            SELECT
                standardized_id,
                security_id,
                symbol,
                cik,
                item_id,
                canonical_code,
                basis,
                period_start,
                period_end,
                fiscal_year,
                fiscal_period,
                unit,
                unit_type,
                value,
                previous_value,
                value_delta,
                value_delta_percent,
                first_value(value) OVER chain AS first_reported_value,
                source_accession,
                filed_date,
                lag(source_accession) OVER chain AS previous_accession,
                lag(available_at) OVER chain AS previous_available_at,
                revision_group_id,
                revision_sequence,
                revision_count,
                update_type,
                is_latest_revision,
                is_value_changed,
                as_of_date,
                available_at,
                run_id,
                source_loaded_at
            FROM fundamental_standardized
            WINDOW chain AS (
                PARTITION BY revision_group_id
                ORDER BY revision_sequence
                ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
            )
        )
        SELECT
            standardized_id AS restatement_event_id,
            security_id,
            symbol,
            cik,
            item_id,
            canonical_code,
            basis,
            period_start,
            period_end,
            fiscal_year,
            fiscal_period,
            unit,
            unit_type,
            value AS restated_value,
            previous_value,
            first_reported_value,
            value_delta,
            value_delta_percent,
            value - first_reported_value AS cumulative_delta,
            source_accession AS restating_accession,
            filed_date AS restating_filed_date,
            previous_accession,
            previous_available_at,
            revision_group_id,
            revision_sequence,
            revision_count,
            update_type,
            is_latest_revision,
            as_of_date,
            available_at,
            run_id,
            source_loaded_at
        FROM chains
        WHERE revision_sequence > 1
          AND coalesce(is_value_changed, false)
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,
            pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            "v_fundamental_restatement_events",
            "view",
            "fundamental_restatement_event",
            "revision_group_id,revision_sequence",
            "One immutable event per standardized revision that changed a previously published value, with restating and superseded filing lineage.",
            '["revision_group_id","revision_sequence"]',
            "available_at gates when the restated vintage may affect a PIT result; previous_available_at retains the superseded vintage. Events are never superseded because each revision is its own natural key.",
        ],
    )
    # Reseed the public contract so the restatements schema, its fields, and
    # every schema digest land in the API catalog tables.
    _seed_public_contract(conn)
    conn.executemany(
        """
        UPDATE api_schema_catalog
        SET schema_sha256=?, updated_at=now()
        WHERE dataset_id=? AND schema_code=? AND schema_version=?
        """,
        [
            (
                _record_schema_sha256(schema),
                dataset.code,
                schema.code,
                schema.version,
            )
            for dataset in DATASETS
            for schema in dataset.schemas
        ],
    )
    _catalog_fields_for_tables(conn, ("v_fundamental_restatement_events",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=298,
        name="restatement_event_surface",
        up=_restatement_event_surface,
    )
]
