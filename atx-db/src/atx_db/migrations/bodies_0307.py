"""Public security-master view, per-schema coverage SLOs, and the measured item-count basis."""

from __future__ import annotations

import duckdb

from ..provider_coverage import DEFAULT_PROVIDER_COVERAGE_SLOS
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _public_schema_coverage_slos(conn: duckdb.DuckDBPyConnection) -> None:
    # DuckDB requires adding the column and its NOT NULL constraint separately.
    conn.execute(
        "ALTER TABLE api_schema_coverage_slo "
        "ADD COLUMN IF NOT EXISTS item_count_basis VARCHAR DEFAULT 'distinct'"
    )
    # Secondary indexes also block DuckDB's ALTER COLUMN dependency check.
    conn.execute("DROP INDEX IF EXISTS idx_api_schema_coverage_slo_active")
    conn.execute("ALTER TABLE api_schema_coverage_slo ALTER COLUMN item_count_basis SET NOT NULL")
    conn.execute(
        "CREATE INDEX idx_api_schema_coverage_slo_active "
        "ON api_schema_coverage_slo(dataset_id,schema_code,is_active,valid_from)"
    )
    conn.execute(
        """
        CREATE OR REPLACE VIEW v_security_master_public AS
        SELECT
            s.security_id,
            s.entity_id,
            s.issuer_id,
            s.primary_symbol,
            s.name,
            s.asset_class,
            s.country,
            s.currency,
            s.active,
            max(CASE WHEN h.id_type = 'CIK' THEN h.id_value END) AS cik,
            max(CASE WHEN h.id_type = 'LEI' THEN h.id_value END) AS lei,
            max(CASE WHEN h.id_type = 'FIGI' THEN h.id_value END) AS figi,
            greatest(
                max(h.as_of_date),
                coalesce(s.last_seen_date, s.first_seen_date, CAST(s.source_loaded_at AS DATE))
            ) AS as_of_date,
            greatest(max(coalesce(h.available_at, h.source_loaded_at)), s.source_loaded_at) AS available_at,
            s.source,
            CAST(NULL AS VARCHAR) AS run_id,
            greatest(max(h.source_loaded_at), s.source_loaded_at) AS source_loaded_at
        FROM securities s
        LEFT JOIN security_identifier_history h
          ON h.security_id = s.security_id
         AND h.id_type IN ('CIK', 'LEI', 'FIGI')
         AND h.valid_to IS NULL
         AND h.is_latest_revision
        GROUP BY
            s.security_id, s.entity_id, s.issuer_id, s.primary_symbol, s.name,
            s.asset_class, s.country, s.currency, s.active, s.last_seen_date,
            s.first_seen_date, s.source, s.source_loaded_at
        """
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO api_schema_coverage_slo (
            dataset_id,schema_code,slo_version,expected_history_start,
            minimum_history_years,minimum_security_count,minimum_item_count,
            maximum_freshness_lag_days,citation,description,item_count_basis,is_active,
            valid_from,valid_to,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,true,TIMESTAMP '1900-01-01',NULL,now())
        """,
        [
            (
                slo.dataset_id,
                slo.schema_code,
                slo.slo_version,
                slo.expected_history_start,
                slo.minimum_history_years,
                slo.minimum_security_count,
                slo.minimum_item_count,
                slo.maximum_freshness_lag_days,
                slo.citation,
                slo.description,
                slo.item_count_basis,
            )
            for slo in DEFAULT_PROVIDER_COVERAGE_SLOS
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "v_security_master_public",
                "serving",
                "security",
                "security_id",
                "Public security-master contract: the securities spine with its open CIK, "
                "LEI and FIGI aliases. Deliberately excludes the internal-only CUSIP that "
                "v_security_master_current exposes.",
                '["security_id"]',
                "as_of_date and available_at are the newest security/identifier input stamps; "
                "the view is current-state, not bitemporal.",
            )
        ],
    )
    _catalog_fields_for_tables(conn, ("v_security_master_public", "api_schema_coverage_slo"))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=307,
        name="public_schema_coverage_slos",
        up=_public_schema_coverage_slos,
    )
]
