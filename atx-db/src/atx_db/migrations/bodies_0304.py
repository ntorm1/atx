"""Point-in-time US-listed universe membership intervals."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _universe_us_listed_membership(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS universe_us_listed_membership (
            membership_id VARCHAR PRIMARY KEY,
            universe_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            valid_from DATE NOT NULL,
            valid_to DATE,
            available_at TIMESTAMP NOT NULL,
            security_type VARCHAR NOT NULL,
            exchange_code VARCHAR NOT NULL,
            has_cik BOOLEAN NOT NULL,
            cik VARCHAR,
            market_cap_decile INTEGER,
            reason VARCHAR NOT NULL,
            rules_json VARCHAR NOT NULL,
            decision_count INTEGER NOT NULL,
            as_of_date DATE NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );

        CREATE INDEX IF NOT EXISTS idx_universe_us_listed_membership_asof
            ON universe_us_listed_membership(universe_id, valid_from, valid_to);
        CREATE INDEX IF NOT EXISTS idx_universe_us_listed_membership_security
            ON universe_us_listed_membership(security_id, valid_from);
        """
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "universe_us_listed_membership",
                "serving",
                "universe_membership",
                "universe_id,security_id,valid_from",
                "Interval-keyed point-in-time US-listed equity universe: one row per "
                "contiguous run of identical (security_type, exchange_code, has_cik, reason) "
                "state for a security. Members with no resolved CIK are retained with "
                "has_cik=false and reason='member_no_cik' so the unresolved tail is counted, "
                "never dropped.",
                '["universe_id","security_id","valid_from"]',
                "valid_from/valid_to are economic dates on the archive trading-session grid; "
                "available_at is the earliest timestamp a consumer could have known the "
                "interval opened. market_cap_decile is the decile AT valid_from only and must "
                "never be read as a per-date attribute; use market_daily_metrics.market_cap "
                "for a dated decile. is_latest_revision is expected to always be true: "
                "refresh_universe_us_listed fully replaces a universe_id's interval set on "
                "every run rather than maintaining a revision chain, so this column carries "
                "no information today but keeps the table's shape consistent with every "
                "other fact table under the current PIT regime.",
            )
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id,source_system_id,name,description,grain,primary_table,
            pit_column,available_at_column,updated_at
        ) VALUES (?,?,?,?,?,?,'as_of_date','available_at',now())
        """,
        [
            (
                "universe_us_listed",
                "atx_derived",
                "US-listed equity universe",
                "Securities with at least one trade in the trailing lookback window on an "
                "eligible US exchange, restricted to common/ADR/REIT/LP security types.",
                "universe_id,security_id,valid_from",
                "universe_us_listed_membership",
            )
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO lake_partition_specs (
            object_name,partition_columns_json,watermark_column,updated_at
        ) VALUES (?,?,'available_at',now())
        """,
        [("universe_us_listed_membership", '["as_of_date"]')],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name,dataset_id,table_name,severity,threshold_value,
            comparator,enabled,failure_status,source,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,now())
        """,
        [
            (
                "universe_us_listed_overlapping_intervals",
                "universe_us_listed",
                "universe_us_listed_membership",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_tier1_parity",
            ),
            (
                "universe_us_listed_missing_decile",
                "universe_us_listed",
                "universe_us_listed_membership",
                "warning",
                0.0,
                "eq",
                True,
                "warning",
                "atx_tier1_parity",
            ),
        ],
    )
    _catalog_fields_for_tables(conn, ("universe_us_listed_membership",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=304,
        name="universe_us_listed_membership",
        up=_universe_us_listed_membership,
    )
]
