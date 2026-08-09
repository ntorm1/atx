"""Merge high-value pre-extraction datasets into a current atx-db warehouse.

The legacy database remains read-only. Each target table is replaced in its own
transaction using the intersection of source and current-schema columns, so a
failure cannot leave a partially copied table.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.finra import FinraShortInterestDataset
from atx_db.ticker_history import TickerHistoryDataset
from atx_db.warehouse import record_source_file

DEFAULT_TABLES = (
    "securities",
    "security_identifier_history",
    "exchange_listings",
    "sec_company_tickers",
    "nasdaq_symbol_directory",
    "nasdaq_listing_events",
    "listing_status_intervals",
    "security_listing_metrics",
    "sec_submissions",
    "sec_company_facts",
    "fundamental_points",
    "fundamental_fact_revisions",
    "fundamental_periods",
    "fundamental_statement_points",
    "fundamental_ttm_points",
    "fundamental_ratios",
    "fundamental_xbrl_metric",
    "shares_outstanding_history",
    "xbrl_taxonomy_packages",
    "xbrl_taxonomy_roles",
    "xbrl_taxonomy_relationships",
    "xbrl_dimension_edges",
    "xbrl_fact_frames",
    "xbrl_filing_contexts",
    "xbrl_filing_dimensions",
    "xbrl_filing_facts",
    "xbrl_concept_catalog",
    "xbrl_validation_results",
    "equity_daily_bars",
    "tbltickerhistory_daily",
    "equity_price_metrics",
    "adjustment_factor_history",
    "daily_adjustment_factors",
    "corporate_actions",
    "corporate_action_dividend_metrics",
    "corporate_action_split_metrics",
    "corporate_action_factor_reconciliation",
    "trading_calendar",
    "macro_series",
    "macro_observations",
    "macro_metrics",
    "finra_short_interest",
    "short_interest_metrics",
    "universes",
    "universe_memberships",
    "feature_values",
    "feature_dependency_edges",
    "alpha_backtest_manifests",
    "alpha_signal_values",
)


def _quote(identifier: str) -> str:
    return '"' + identifier.replace('"', '""') + '"'


def _integer(value: object) -> int:
    if not isinstance(value, int):
        raise TypeError(f"Expected an integer count, received {type(value).__name__}")
    return value


def _columns(store: DuckDBStore, database_name: str, table_name: str) -> list[str]:
    return [
        str(row[0])
        for row in store.con.execute(
            """
            SELECT column_name
            FROM duckdb_columns()
            WHERE database_name = ? AND schema_name = 'main' AND table_name = ?
            ORDER BY column_index
            """,
            [database_name, table_name],
        ).fetchall()
    ]


def merge_table(store: DuckDBStore, table_name: str, *, replace: bool) -> dict[str, object]:
    database_row = store.con.execute("SELECT current_database()").fetchone()
    if database_row is None:
        raise RuntimeError("Could not resolve the target database name")
    target_columns = _columns(store, str(database_row[0]), table_name)
    source_columns = set(_columns(store, "legacy", table_name))
    columns = [column for column in target_columns if column in source_columns]
    if not target_columns:
        raise RuntimeError(f"Target table does not exist: {table_name}")
    if not source_columns:
        raise RuntimeError(f"Legacy table does not exist: {table_name}")
    if not columns:
        raise RuntimeError(f"No compatible columns for legacy table: {table_name}")
    column_sql = ", ".join(_quote(column) for column in columns)
    with store.transaction():
        if replace:
            store.con.execute(f"DELETE FROM main.{_quote(table_name)}")
        store.con.execute(
            f"""
            INSERT INTO main.{_quote(table_name)} ({column_sql})
            SELECT {column_sql}
            FROM legacy.main.{_quote(table_name)}
            """
        )
    source_row = store.con.execute(f"SELECT count(*) FROM legacy.main.{_quote(table_name)}").fetchone()
    target_row = store.con.execute(f"SELECT count(*) FROM main.{_quote(table_name)}").fetchone()
    if source_row is None or target_row is None:
        raise RuntimeError(f"Could not verify copied table: {table_name}")
    return {
        "table": table_name,
        "source_rows": int(source_row[0]),
        "target_rows": int(target_row[0]),
        "copied_columns": len(columns),
        "target_columns": len(target_columns),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Merge legacy atx-db facts into a current warehouse")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--legacy-db-path", type=Path, required=True)
    parser.add_argument("--table", action="append", dest="tables")
    parser.add_argument("--append", action="store_true")
    parser.add_argument("--continue-on-error", action="store_true")
    args = parser.parse_args()
    legacy_path = args.legacy_db_path.resolve()
    if not legacy_path.is_file():
        raise FileNotFoundError(legacy_path)
    escaped = legacy_path.as_posix().replace("'", "''")
    results: list[dict[str, object]] = []
    failures: list[dict[str, str]] = []
    with DuckDBStore(args.db_path) as store:
        # These high-volume raw tables are intentionally lazy in a new
        # warehouse. Activate them before inspecting compatible columns so the
        # default full merge works in a single pass.
        TickerHistoryDataset().ensure_schema(store)
        FinraShortInterestDataset().ensure_schema(store)
        store.con.execute(f"ATTACH '{escaped}' AS legacy (READ_ONLY)")
        try:
            for table_name in tuple(args.tables or DEFAULT_TABLES):
                try:
                    result = merge_table(store, table_name, replace=not args.append)
                    results.append(result)
                    print(json.dumps(result, sort_keys=True), flush=True)
                except Exception as exc:
                    failure = {"table": table_name, "error": str(exc)}
                    failures.append(failure)
                    print(json.dumps(failure, sort_keys=True), flush=True)
                    if not args.continue_on_error:
                        raise
        finally:
            store.con.execute("DETACH legacy")
        record_source_file(
            store,
            dataset_id="legacy_warehouse_merge",
            source_url=f"local:{legacy_path.name}",
            cache_path=legacy_path,
            status="loaded" if not failures else "partial",
            metadata={"tables": results, "failures": failures, "replace": not args.append},
        )
    print(
        json.dumps(
            {
                "tables_copied": len(results),
                "tables_failed": len(failures),
                "source_rows": sum(_integer(result["source_rows"]) for result in results),
                "target_rows": sum(_integer(result["target_rows"]) for result in results),
            },
            sort_keys=True,
        )
    )
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
