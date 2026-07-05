"""PF3-S4 migration bodies: PIT universe and price-backfill surfaces."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import (
    _catalog_fields_for_tables,
    _pf3_s2_schema_contract_semantics,
)


def _pf3_s4_universe_membership_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S4 S4-0: governed interval-keyed PIT universe decisions."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS universe_membership (
            universe_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            valid_from DATE NOT NULL,
            valid_to DATE,
            as_of_date DATE NOT NULL,
            is_member BOOLEAN NOT NULL DEFAULT true,
            reason VARCHAR NOT NULL,
            rules_json VARCHAR NOT NULL,
            decision_count INTEGER NOT NULL DEFAULT 1,
            available_at TIMESTAMP,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (universe_id, security_id, valid_from, source)
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'universe_membership',
            'atx_warehouse',
            'Governed PIT universe membership decisions',
            'Interval-keyed common-equity/listing/liquidity universe decisions reconciled to the legacy universe_memberships snapshot surface.',
            'universe_id,security_id,valid_from',
            'universe_membership',
            'as_of_date',
            'available_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'universe_membership',
            'gold',
            'universe_member_interval',
            'universe_id,security_id,valid_from',
            'Governed interval-keyed PIT universe decision surface. Rows carry both in-universe and explicitly-screened-out decisions so downstream coverage checks can distinguish excluded from undecided securities.',
            '["universe_id","security_id","valid_from","source"]',
            'Use valid_from/valid_to plus available_at for as-of-safe membership. universe_membership_asof returns only is_member=true rows and caps validity at the query date to avoid leaking future interval closure.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("universe_membership",))
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        VALUES
            ('universe_membership', 'universe_id', 'identifier', 'Research universe identifier.', false, NULL, 'universes.universe_id', now()),
            ('universe_membership', 'security_id', 'identifier', 'Warehouse security identifier.', false, NULL, 'securities.security_id', now()),
            ('universe_membership', 'symbol', 'identifier', 'Display symbol used when the decision was computed.', true, NULL, 'equity_daily_bars.symbol', now()),
            ('universe_membership', 'valid_from', 'date', 'First date covered by this membership decision interval.', false, 'date', NULL, now()),
            ('universe_membership', 'valid_to', 'date', 'Last loaded date covered by this membership decision interval; NULL means open-ended only when the source truly emits an open interval.', true, 'date', NULL, now()),
            ('universe_membership', 'as_of_date', 'date', 'Decision as-of date, equal to valid_from for the interval row.', false, 'date', NULL, now()),
            ('universe_membership', 'is_member', 'flag', 'True when the security passed all universe screens for this interval; false rows are explicit exclusion decisions.', false, 'boolean', NULL, now()),
            ('universe_membership', 'reason', 'category', 'Decision reason such as member, not_common_equity, inactive_listing, or liquidity_screen_fail.', false, NULL, NULL, now()),
            ('universe_membership', 'rules_json', 'json', 'JSON payload describing the universe screen thresholds and compatibility snapshot source.', false, NULL, NULL, now()),
            ('universe_membership', 'decision_count', 'count', 'Number of daily decisions compressed into this interval row.', false, 'count', NULL, now()),
            ('universe_membership', 'available_at', 'timestamp', 'Timestamp when the interval-start decision became visible to as-of readers.', true, 'timestamp', NULL, now()),
            ('universe_membership', 'source', 'identifier', 'Builder/source that produced this universe decision interval.', false, NULL, NULL, now()),
            ('universe_membership', 'run_id', 'identifier', 'Optional build or backfill run id.', true, NULL, NULL, now()),
            ('universe_membership', 'is_latest_revision', 'flag', 'Latest-revision marker for schema-contract and quality-gate consistency.', false, 'boolean', NULL, now()),
            ('universe_membership', 'source_loaded_at', 'timestamp', 'Warehouse timestamp when the interval row was loaded.', false, 'timestamp', NULL, now())
        """
    )
    _pf3_s2_schema_contract_semantics(conn)

    from .schema_contract import (
        SCHEMA_CONTRACT_VERSION,
        assert_schema_contract_version,
        build_contract_manifest,
        schema_contract_sha256,
    )

    manifest = build_contract_manifest(conn)
    manifest_sha256 = schema_contract_sha256(manifest)
    conn.execute(
        """
        UPDATE schema_contract_version
        SET manifest_sha256 = ?
        WHERE version = ?
        """,
        [manifest_sha256, SCHEMA_CONTRACT_VERSION],
    )
    assert_schema_contract_version(conn, manifest=manifest)


MIGRATIONS: list[Migration] = [
    Migration(
        version=140,
        name="pf3_s4_universe_membership_schema_catalog",
        up=_pf3_s4_universe_membership_schema_catalog,
    ),
]
