"""PF3-S4 migration bodies: PIT universe and price-backfill surfaces."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import (
    _catalog_fields_for_tables,
    _pf3_s2_schema_contract_semantics,
)


def _refresh_schema_contract_v2_pin(conn: duckdb.DuckDBPyConnection) -> None:
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
    _refresh_schema_contract_v2_pin(conn)


def _pf3_s4_price_backfill_partition_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S4 S4-1: per-partition price-backfill metadata."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS price_backfill_partition (
            dataset_id VARCHAR NOT NULL,
            partition_key VARCHAR NOT NULL,
            window_lo DATE NOT NULL,
            window_hi DATE NOT NULL,
            status VARCHAR NOT NULL,
            rows_loaded BIGINT NOT NULL DEFAULT 0,
            watermark_after VARCHAR,
            min_trade_date DATE,
            max_trade_date DATE,
            source VARCHAR NOT NULL,
            archive_path VARCHAR,
            source_file_sha256 VARCHAR,
            backfill_run_id VARCHAR,
            details_json VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (dataset_id, partition_key)
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
            'bulk_daily_bars_backfill',
            'bulk_bars_2015plus',
            'Bulk daily bars backfill partitions',
            'PF3-S4 partition metadata for historical daily-bar backfills executed through the PF3-S1 backfill engine.',
            'dataset_id,partition_key',
            'price_backfill_partition',
            'window_lo',
            'updated_at',
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
            'price_backfill_partition',
            'control',
            'price_backfill_partition',
            'dataset_id,partition_key',
            'Operator-facing partition metadata for PF3-S4 historical daily-bar backfills. The generic PF3-S1 backfill tables remain the execution source of truth; this table records price-specific source/archive and row-count evidence.',
            '["dataset_id","partition_key"]',
            'Rows are partition-control metadata. window_lo/window_hi identify the half-open date partition; updated_at/source_loaded_at record when the partition evidence was written.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("price_backfill_partition",))
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        VALUES
            ('price_backfill_partition', 'dataset_id', 'identifier', 'Backfill dataset id, currently bulk_daily_bars_backfill.', false, NULL, NULL, now()),
            ('price_backfill_partition', 'partition_key', 'identifier', 'PF3-S1 partition key for the price backfill window.', false, NULL, 'backfill_watermark.partition_key', now()),
            ('price_backfill_partition', 'window_lo', 'date', 'Inclusive lower bound of the backfill partition.', false, 'date', 'backfill_watermark.window_lo', now()),
            ('price_backfill_partition', 'window_hi', 'date', 'Exclusive upper bound of the backfill partition.', false, 'date', 'backfill_watermark.window_hi', now()),
            ('price_backfill_partition', 'status', 'category', 'Partition load status recorded by the price backfill client.', false, NULL, NULL, now()),
            ('price_backfill_partition', 'rows_loaded', 'count', 'Rows loaded by BulkBarsDataset for this partition execution.', false, 'count', NULL, now()),
            ('price_backfill_partition', 'watermark_after', 'identifier', 'Watermark value returned to the generic PF3-S1 backfill engine.', true, NULL, 'backfill_watermark.watermark_after', now()),
            ('price_backfill_partition', 'min_trade_date', 'date', 'Minimum trade date loaded in this partition execution.', true, 'date', 'equity_daily_bars.trade_date', now()),
            ('price_backfill_partition', 'max_trade_date', 'date', 'Maximum trade date loaded in this partition execution.', true, 'date', 'equity_daily_bars.trade_date', now()),
            ('price_backfill_partition', 'source', 'identifier', 'Bulk bar source name.', false, NULL, NULL, now()),
            ('price_backfill_partition', 'archive_path', 'path', 'CSV or ZIP archive path used by the partition.', true, NULL, NULL, now()),
            ('price_backfill_partition', 'source_file_sha256', 'identifier', 'SHA256 of the source archive when hashing is enabled.', true, NULL, 'raw_source_files.sha256', now()),
            ('price_backfill_partition', 'backfill_run_id', 'identifier', 'PF3-S1 backfill run id that executed this partition.', true, NULL, 'backfill_run.backfill_run_id', now()),
            ('price_backfill_partition', 'details_json', 'json', 'Full JSON details returned by the price backfill client.', true, NULL, NULL, now()),
            ('price_backfill_partition', 'updated_at', 'timestamp', 'Timestamp when this partition evidence row was last written.', false, 'timestamp', NULL, now()),
            ('price_backfill_partition', 'source_loaded_at', 'timestamp', 'Warehouse timestamp when this partition evidence row was inserted.', false, 'timestamp', NULL, now())
        """
    )
    _refresh_schema_contract_v2_pin(conn)


def _pf3_s4_price_fundamental_overlap_view_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S4 S4-2: dense price/fundamental overlap evidence view."""

    conn.execute(
        """
        CREATE OR REPLACE VIEW v_price_fundamental_overlap AS
        WITH price_days AS (
            SELECT
                security_id,
                max(symbol) AS symbol,
                trade_date,
                max(available_at) AS price_available_at,
                count(*) AS price_row_count
            FROM equity_daily_bars
            WHERE security_id IS NOT NULL
              AND trade_date IS NOT NULL
              AND close IS NOT NULL
              AND close > 0
            GROUP BY security_id, trade_date
        ),
        member_price_days AS (
            SELECT
                u.universe_id,
                p.security_id,
                coalesce(u.symbol, p.symbol) AS symbol,
                p.trade_date,
                p.price_available_at,
                p.price_row_count
            FROM price_days p
            JOIN universe_membership u
              ON u.security_id = p.security_id
             AND u.valid_from <= p.trade_date
             AND (u.valid_to IS NULL OR u.valid_to >= p.trade_date)
             AND u.as_of_date <= p.trade_date
             AND u.is_member
             AND u.is_latest_revision
             AND (
                 u.available_at IS NULL
                 OR p.price_available_at IS NULL
                 OR u.available_at <= p.price_available_at
             )
        ),
        overlap_days AS (
            SELECT
                mp.*,
                EXISTS (
                    SELECT 1
                    FROM fundamental_points f
                    WHERE f.security_id = mp.security_id
                      AND f.period_end IS NOT NULL
                      AND f.period_end <= mp.trade_date
                      AND f.value IS NOT NULL
                      AND (
                          f.available_at IS NULL
                          OR mp.price_available_at IS NULL
                          OR f.available_at <= mp.price_available_at
                      )
                ) AS has_visible_fundamental
            FROM member_price_days mp
        )
        SELECT
            universe_id,
            CAST(date_trunc('month', trade_date) AS DATE) AS overlap_month,
            min(trade_date) AS first_trade_date,
            max(trade_date) AS last_trade_date,
            count(*) AS universe_price_days,
            sum(CASE WHEN has_visible_fundamental THEN 1 ELSE 0 END) AS price_fundamental_days,
            count(DISTINCT security_id) AS universe_priced_security_count,
            count(DISTINCT CASE WHEN has_visible_fundamental THEN security_id ELSE NULL END)
                AS overlapped_security_count,
            sum(price_row_count) AS price_row_count,
            CASE
                WHEN count(*) = 0 THEN NULL
                ELSE CAST(sum(CASE WHEN has_visible_fundamental THEN 1 ELSE 0 END) AS DOUBLE)
                    / CAST(count(*) AS DOUBLE)
            END AS overlap_ratio
        FROM overlap_days
        GROUP BY universe_id, CAST(date_trunc('month', trade_date) AS DATE)
        ORDER BY universe_id, overlap_month
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'price_fundamental_overlap',
            'atx_warehouse',
            'Price/fundamental overlap density',
            'Monthly evidence that governed PIT universe security-days have both price bars and visible fundamental facts.',
            'universe_id,overlap_month',
            'v_price_fundamental_overlap',
            'overlap_month',
            NULL,
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
            'v_price_fundamental_overlap',
            'view',
            'price_fundamental_overlap',
            'universe_id,overlap_month',
            'Monthly overlap-density evidence over governed PIT universe member price-days and visible fundamental_points rows.',
            '["universe_id","overlap_month"]',
            'Universe membership and fundamental availability are both gated by the price day availability timestamp. Use this as density evidence, not as a substitute for formula-specific valuation inputs.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("v_price_fundamental_overlap",))
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        VALUES
            ('v_price_fundamental_overlap', 'universe_id', 'identifier', 'Governed universe identifier.', false, NULL, 'universe_membership.universe_id', now()),
            ('v_price_fundamental_overlap', 'overlap_month', 'date', 'Calendar month for the grouped overlap evidence.', false, 'month', 'equity_daily_bars.trade_date', now()),
            ('v_price_fundamental_overlap', 'first_trade_date', 'date', 'First priced universe-member trading day in the grouped month.', true, 'date', 'equity_daily_bars.trade_date', now()),
            ('v_price_fundamental_overlap', 'last_trade_date', 'date', 'Last priced universe-member trading day in the grouped month.', true, 'date', 'equity_daily_bars.trade_date', now()),
            ('v_price_fundamental_overlap', 'universe_price_days', 'count', 'Count of governed universe member security-days with a positive close price.', false, 'count', 'equity_daily_bars', now()),
            ('v_price_fundamental_overlap', 'price_fundamental_days', 'count', 'Count of governed universe member price-days with at least one visible fundamental point.', true, 'count', 'fundamental_points', now()),
            ('v_price_fundamental_overlap', 'universe_priced_security_count', 'count', 'Distinct governed universe securities with positive-price days in the month.', false, 'count', 'equity_daily_bars.security_id', now()),
            ('v_price_fundamental_overlap', 'overlapped_security_count', 'count', 'Distinct governed universe securities with both price and visible fundamental evidence in the month.', true, 'count', 'fundamental_points.security_id', now()),
            ('v_price_fundamental_overlap', 'price_row_count', 'count', 'Underlying price rows represented by the grouped security-days.', true, 'count', 'equity_daily_bars', now()),
            ('v_price_fundamental_overlap', 'overlap_ratio', 'ratio', 'price_fundamental_days divided by universe_price_days.', true, 'ratio', NULL, now())
        """
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=140,
        name="pf3_s4_universe_membership_schema_catalog",
        up=_pf3_s4_universe_membership_schema_catalog,
    ),
    Migration(
        version=141,
        name="pf3_s4_price_backfill_partition_schema_catalog",
        up=_pf3_s4_price_backfill_partition_schema_catalog,
    ),
    Migration(
        version=142,
        name="pf3_s4_price_fundamental_overlap_view_catalog",
        up=_pf3_s4_price_fundamental_overlap_view_catalog,
    ),
]
