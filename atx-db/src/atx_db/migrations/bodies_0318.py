"""Permit bounded atomic publication of the core fundamentals tables."""

from __future__ import annotations

import duckdb

from ._runner import Migration

_OPTIONAL_INDEXES = {
    "fundamental_fact_revisions": (
        "idx_fundamental_fact_revisions_group",
        "idx_fundamental_fact_revisions_security",
        "idx_fundamental_fact_revisions_latest",
    ),
    "fundamental_statement_points": (
        "idx_fundamental_statement_points_metric_asof",
        "idx_fundamental_statement_points_security",
        "idx_fundamental_statement_points_revision",
        "idx_fundamental_statement_points_source_accession",
    ),
    "fundamental_periods": (
        "idx_fundamental_periods_security",
        "idx_fundamental_periods_group",
        "idx_fundamental_periods_type",
    ),
    "fundamental_ttm_points": (
        "idx_fundamental_ttm_points_metric_asof",
        "idx_fundamental_ttm_points_security",
        "idx_fundamental_ttm_points_revision",
    ),
    "fundamental_calendar_map": (
        "idx_fundamental_calendar_map_period",
        "idx_fundamental_calendar_map_overlap",
        "idx_fundamental_calendar_map_53_week",
    ),
    "fundamental_calendar_ttm": (
        "idx_fundamental_calendar_ttm_window",
        "idx_fundamental_calendar_ttm_latest",
    ),
    "fundamental_standardized": (
        "idx_fundamental_standardized_item",
        "idx_fundamental_standardized_security",
        "idx_fundamental_standardized_asof",
        "idx_fundamental_standardized_latest",
        "idx_fundamental_standardized_revision",
        "idx_fundamental_standardized_validity",
    ),
    "fundamental_standardization_exception": (
        "idx_fundamental_std_exception_security",
        "idx_fundamental_std_exception_reason",
        "idx_fundamental_std_exception_asof",
    ),
}


def _bounded_fundamentals_publication(conn: duckdb.DuckDBPyConnection) -> None:
    """Remove only governed optional indexes; retain every PK/UNIQUE contract."""
    for table, indexes in _OPTIONAL_INDEXES.items():
        for index in indexes:
            row = conn.execute("""
                SELECT table_name, is_unique, is_primary FROM duckdb_indexes()
                WHERE database_name = current_database() AND schema_name = current_schema()
                  AND index_name = ?
            """, [index]).fetchone()
            if row is not None and (row[0] != table or row[1] or row[2]):
                raise RuntimeError(f"0318 refuses to remove a non-optional index: {index}")
            conn.execute(f"DROP INDEX IF EXISTS {index}")


MIGRATIONS = [
    Migration(version=318, name="bounded_fundamentals_publication", up=_bounded_fundamentals_publication)
]
