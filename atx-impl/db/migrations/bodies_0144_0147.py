"""PF3-S5 migration bodies: fundamentals completeness for valuation inputs."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _pf3_s5_share_count_extensions(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S5 S5-0: float, treasury, and per-share-class metadata."""

    statements = (
        "ALTER TABLE shares_outstanding_history ADD COLUMN IF NOT EXISTS share_class VARCHAR",
        "ALTER TABLE shares_outstanding_history ADD COLUMN IF NOT EXISTS share_count_category VARCHAR",
    )
    for statement in statements:
        conn.execute(statement)
    conn.execute(
        """
        UPDATE shares_outstanding_history
        SET share_count_category = CASE
                WHEN share_count_type IN ('shares_outstanding', 'shares_basic_avg', 'shares_diluted_avg')
                    THEN 'consolidated'
                WHEN share_count_type IN ('float', 'treasury')
                    THEN 'float_treasury'
                WHEN share_count_type LIKE 'class_%'
                    THEN 'share_class'
                ELSE share_count_category
            END,
            share_class = CASE
                WHEN share_class IS NULL AND share_count_type LIKE 'class_%'
                    THEN upper(substr(share_count_type, 7))
                ELSE share_class
            END
        WHERE share_count_category IS NULL
           OR (share_class IS NULL AND share_count_type LIKE 'class_%')
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'shares_outstanding_history',
            'sec_edgar',
            'Shares outstanding history',
            'PIT share-count history derived from SEC XBRL shares outstanding/basic average/diluted average, plus PF3-S5 float, treasury, and share-class facts when disclosed as share counts.',
            'security_id,share_count_type,effective_date,accession',
            'shares_outstanding_history',
            'effective_date',
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
            'shares_outstanding_history',
            'silver',
            'shares_outstanding',
            'source,security_id,share_count_type,effective_date,accession_number',
            'PIT share-count history derived from normalized SEC XBRL share-count facts, including float, treasury, and common-share-class counts where the source fact is share-denominated.',
            '["share_history_id"]',
            'Use effective_date/as_of_date/available_at for as-of-safe market-cap, float, treasury, and share-class research. float rows must not exceed the same-vintage shares_outstanding row when that row exists.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("shares_outstanding_history",))
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        VALUES
            ('shares_outstanding_history', 'share_count_type', 'category', 'Share count type: shares_outstanding, shares_basic_avg, shares_diluted_avg, float, treasury, or class_a/class_b/class_c/class_d.', false, NULL, 'fundamental_statement_points.canonical_metric', now()),
            ('shares_outstanding_history', 'share_class', 'category', 'Common share-class label for per-class rows, such as A/B/C/D; NULL for consolidated, float, and treasury rows.', true, NULL, 'fundamental_statement_points.canonical_metric', now()),
            ('shares_outstanding_history', 'share_count_category', 'category', 'Share-count family: consolidated, float_treasury, or share_class.', true, NULL, NULL, now()),
            ('shares_outstanding_history', 'share_count', 'measure', 'Non-negative share count in raw shares. Treasury rows are treasury shares; float rows must be less than or equal to same-vintage shares_outstanding when disclosed.', false, 'shares', 'fundamental_statement_points.value', now())
        """
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=144,
        name="pf3_s5_share_count_extensions",
        up=_pf3_s5_share_count_extensions,
    ),
]
