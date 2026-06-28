#!/usr/bin/env python
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from dataclasses import asdict
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore, JobManager
from db.fundamentals import COMPANY_FACT_SYMBOL_SOURCES
from db.lake import DEFAULT_EXPORT_OBJECTS, DEFAULT_LAKE_ROOT, LakehouseExporter
from db.quality import run_warehouse_quality_checks
from db.ticker_history import (
    DEFAULT_TICKER_HISTORY_ZIP,
    TickerHistoryArchiveProfileOptions,
    profile_ticker_history_archive,
)
from db.warehouse import symbol_key
from db.watermarks import refresh_warehouse_watermarks
from db.xbrl_taxonomy import DEFAULT_XBRL_TAXONOMY_PACKAGE_URLS


LIQUID40_SYMBOLS = (
    "AAPL",
    "ADBE",
    "AMZN",
    "BA",
    "BAC",
    "C",
    "CAT",
    "COST",
    "CSCO",
    "CVX",
    "DIS",
    "F",
    "FDX",
    "GE",
    "GM",
    "GOOG",
    "GS",
    "HD",
    "IBM",
    "INTC",
    "JNJ",
    "JPM",
    "KO",
    "MCD",
    "MMM",
    "MRK",
    "MSFT",
    "NKE",
    "NVDA",
    "ORCL",
    "PEP",
    "PFE",
    "PG",
    "QCOM",
    "T",
    "UPS",
    "VZ",
    "WFC",
    "WMT",
    "XOM",
)

PRESETS = {
    "aapl": ("AAPL",),
    "liquid40": LIQUID40_SYMBOLS,
}

CORE_JOB_SUFFIXES = (
    "daily_bars",
    "corporate_actions",
    "trading_calendar",
    "universe_memberships",
    "equity_daily_features",
)

CORE_ROW_COUNT_TABLES = (
    "etl_job_definitions",
    "etl_job_runs",
    "dataset_runs",
    "raw_source_files",
    "raw_source_artifacts",
    "security_identifier_history",
    "exchange_listings",
    "tbltickerhistory_daily",
    "equity_daily_bars",
    "corporate_actions",
    "trading_calendar",
    "universes",
    "universe_memberships",
    "feature_definitions",
    "feature_values",
    "feature_set_catalog",
    "feature_dependency_edges",
    "feature_build_manifests",
    "alpha_expression_catalog",
    "alpha_signal_values",
    "alpha_backtest_manifests",
    "xbrl_taxonomy_packages",
    "xbrl_taxonomy_roles",
    "xbrl_taxonomy_relationships",
    "xbrl_dimension_edges",
    "xbrl_fact_frames",
    "xbrl_filing_contexts",
    "xbrl_filing_dimensions",
    "xbrl_filing_facts",
    "lake_export_runs",
    "lake_export_files",
    "dataset_watermarks",
    "data_quality_checks",
)


def parse_date(value: str | None) -> dt.date | None:
    if value in (None, ""):
        return None
    return dt.date.fromisoformat(value)


def parse_symbols_text(value: str | None) -> list[str]:
    if not value:
        return []
    normalized = value.replace("\n", ",").replace("\t", ",").replace(" ", ",")
    return [symbol for symbol in (symbol_key(part) for part in normalized.split(",")) if symbol]


def load_symbols_file(path: Path | None) -> list[str]:
    if path is None:
        return []
    symbols: list[str] = []
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        line = line.split("#", 1)[0].strip()
        symbols.extend(parse_symbols_text(line))
    return symbols


def unique_symbols(symbols: list[str]) -> tuple[str, ...]:
    return tuple(sorted({symbol for symbol in symbols if symbol}))


def resolve_symbols(args: argparse.Namespace) -> tuple[str, ...]:
    explicit = [*parse_symbols_text(args.symbols), *load_symbols_file(args.symbols_file)]
    if explicit:
        return unique_symbols(explicit)
    return unique_symbols(list(PRESETS[args.preset]))


def comma_tuple(value: str | None) -> tuple[str, ...] | None:
    if not value:
        return None
    return tuple(part.strip() for part in value.split(",") if part.strip())


def clean_params(params: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in params.items() if value is not None}


def effective_ticker_max_chunks(args: argparse.Namespace) -> int | None:
    return None if args.full_ticker_history else args.ticker_max_chunks


def job_name(prefix: str, suffix: str) -> str:
    cleaned = prefix.strip().strip("_")
    return f"{cleaned}_{suffix}" if cleaned else suffix


def dataset_result_payload(result: Any) -> dict[str, Any]:
    return {
        "dataset_id": result.dataset_id,
        "rows_loaded": result.rows_loaded,
        "source": result.source,
        "run_id": result.run_id,
        "details": result.details,
    }


def register_jobs(manager: JobManager, args: argparse.Namespace, symbols: tuple[str, ...]) -> list[str]:
    retry_policy = {
        "max_retries": args.max_retries,
        "retry_delay_seconds": args.retry_delay_seconds,
    }
    names = {suffix: job_name(args.job_prefix, suffix) for suffix in CORE_JOB_SUFFIXES}

    manager.register_job(
        job_name=names["daily_bars"],
        dataset_id="tbltickerhistory_daily",
        params=clean_params(
            {
                "zip_path": str(args.ticker_history_zip),
                "symbols": symbols,
                "start_date": args.start_date,
                "end_date": args.end_date,
                "chunk_size": args.ticker_chunk_size,
                "max_chunks": effective_ticker_max_chunks(args),
                "compute_source_hash": args.compute_source_hash,
            }
        ),
        **retry_policy,
    )
    manager.register_job(
        job_name=names["corporate_actions"],
        dataset_id="corporate_actions",
        dependencies=[names["daily_bars"]],
        **retry_policy,
    )
    manager.register_job(
        job_name=names["trading_calendar"],
        dataset_id="trading_calendar",
        dependencies=[names["daily_bars"]],
        **retry_policy,
    )
    manager.register_job(
        job_name=names["universe_memberships"],
        dataset_id="universe_memberships",
        params=clean_params(
            {
                "universe_id": args.universe_id,
                "name": args.universe_name,
                "description": args.universe_description,
                "symbols": symbols,
                "start_date": args.start_date,
                "end_date": args.end_date,
                "lookback_days": args.universe_lookback_days,
                "min_history_days": args.universe_min_history_days,
                "min_price": args.universe_min_price,
                "min_dollar_volume": args.universe_min_dollar_volume,
            }
        ),
        dependencies=[names["daily_bars"]],
        **retry_policy,
    )
    manager.register_job(
        job_name=names["equity_daily_features"],
        dataset_id="equity_daily_features",
        params=clean_params(
            {
                "symbols": symbols,
                "feature_set": args.feature_set,
                "min_rows": args.feature_min_rows,
            }
        ),
        dependencies=[names["daily_bars"]],
        **retry_policy,
    )

    ordered = [
        names["daily_bars"],
        names["corporate_actions"],
        names["trading_calendar"],
        names["universe_memberships"],
        names["equity_daily_features"],
    ]

    if args.include_sec_fundamentals:
        company_facts = job_name(args.job_prefix, "sec_company_facts")
        xbrl_taxonomy = job_name(args.job_prefix, "xbrl_taxonomy")
        fundamental_features = job_name(args.job_prefix, "sec_fundamental_features")
        manager.register_job(
            job_name=company_facts,
            dataset_id="sec_company_facts",
            params=clean_params(
                {
                    "symbols": symbols,
                    "symbol_source": args.sec_company_facts_symbol_source,
                    "symbol_limit": args.sec_company_facts_symbol_limit,
                    "universe_id": args.sec_company_facts_universe_id,
                    "as_of_date": args.sec_company_facts_as_of_date,
                }
            ),
            **retry_policy,
        )
        manager.register_job(
            job_name=xbrl_taxonomy,
            dataset_id="xbrl_taxonomy",
            params=clean_params(
                {
                    "package_urls": comma_tuple(args.xbrl_taxonomy_urls),
                    "request_timeout": args.xbrl_request_timeout,
                    "user_agent": args.xbrl_user_agent,
                }
            ),
            dependencies=[company_facts],
            **retry_policy,
        )
        manager.register_job(
            job_name=fundamental_features,
            dataset_id="sec_fundamental_features",
            params=clean_params(
                {
                    "symbols": symbols,
                    "feature_set": args.fundamental_feature_set,
                    "start_date": args.start_date,
                    "end_date": args.end_date,
                }
            ),
            dependencies=[company_facts, names["daily_bars"]],
            **retry_policy,
        )
        ordered.extend([company_facts, xbrl_taxonomy, fundamental_features])

    if args.include_sec_submissions:
        submissions = job_name(args.job_prefix, "sec_submissions")
        xbrl_contexts = job_name(args.job_prefix, "xbrl_filing_contexts")
        manager.register_job(
            job_name=submissions,
            dataset_id="sec_submissions",
            params=clean_params({"symbols": symbols, "forms": comma_tuple(args.sec_forms)}),
            **retry_policy,
        )
        ordered.append(submissions)
        if args.include_xbrl_filing_contexts:
            manager.register_job(
                job_name=xbrl_contexts,
                dataset_id="xbrl_filing_contexts",
                params=clean_params(
                    {
                        "symbols": symbols,
                        "forms": comma_tuple(args.xbrl_context_forms),
                        "max_filings": args.xbrl_context_max_filings,
                        "max_filings_per_symbol": args.xbrl_context_max_filings_per_symbol,
                        "request_timeout": args.xbrl_context_request_timeout,
                        "user_agent": args.xbrl_context_user_agent,
                    }
                ),
                dependencies=[submissions],
                **retry_policy,
            )
            ordered.append(xbrl_contexts)

    return ordered


def table_or_view_exists(store: DuckDBStore, name: str) -> bool:
    return bool(
        store.con.execute(
            """
            SELECT count(*)
            FROM (
                SELECT table_name AS object_name
                FROM duckdb_tables()
                WHERE schema_name = 'main'
                UNION ALL
                SELECT view_name AS object_name
                FROM duckdb_views()
                WHERE schema_name = 'main'
            ) objects
            WHERE object_name = ?
            """,
            [name],
        ).fetchone()[0]
    )


def collect_row_counts(store: DuckDBStore, tables: tuple[str, ...] = CORE_ROW_COUNT_TABLES) -> dict[str, int]:
    counts: dict[str, int] = {}
    for table in tables:
        if table_or_view_exists(store, table):
            quoted = '"' + table.replace('"', '""') + '"'
            counts[table] = int(store.con.execute(f"SELECT count(*) FROM {quoted}").fetchone()[0])
    return counts


def collect_curated_summaries(store: DuckDBStore) -> dict[str, Any]:
    summaries: dict[str, Any] = {}
    if table_or_view_exists(store, "equity_daily_bars"):
        row = store.con.execute(
            """
            SELECT
                count(*) AS rows,
                count(DISTINCT symbol) AS symbols,
                min(trade_date) AS min_trade_date,
                max(trade_date) AS max_trade_date
            FROM equity_daily_bars
            """
        ).fetchone()
        summaries["equity_daily_bars"] = {
            "rows": int(row[0]),
            "symbols": int(row[1]),
            "min_trade_date": row[2],
            "max_trade_date": row[3],
        }
    if table_or_view_exists(store, "universe_memberships"):
        rows = store.con.execute(
            """
            SELECT
                universe_id,
                count(*) AS rows,
                count(DISTINCT security_id) AS securities,
                min(as_of_date) AS min_as_of_date,
                max(as_of_date) AS max_as_of_date,
                count(DISTINCT run_id) AS run_count
            FROM universe_memberships
            GROUP BY 1
            ORDER BY 1
            """
        ).fetchall()
        summaries["universes"] = [
            {
                "universe_id": row[0],
                "rows": int(row[1]),
                "securities": int(row[2]),
                "min_as_of_date": row[3],
                "max_as_of_date": row[4],
                "run_count": int(row[5]),
            }
            for row in rows
        ]
    if table_or_view_exists(store, "feature_values"):
        rows = store.con.execute(
            """
            SELECT
                feature_set,
                count(*) AS rows,
                count(DISTINCT feature_name) AS features,
                count(DISTINCT security_id) AS securities,
                min(as_of_date) AS min_as_of_date,
                max(as_of_date) AS max_as_of_date
            FROM feature_values
            GROUP BY 1
            ORDER BY 1
            """
        ).fetchall()
        summaries["feature_sets"] = [
            {
                "feature_set": row[0],
                "rows": int(row[1]),
                "features": int(row[2]),
                "securities": int(row[3]),
                "min_as_of_date": row[4],
                "max_as_of_date": row[5],
            }
            for row in rows
        ]
    if table_or_view_exists(store, "xbrl_filing_contexts"):
        rows = store.con.execute(
            """
            SELECT
                coalesce(t.ticker, c.security_id) AS symbol,
                count(DISTINCT c.accession_number) AS filings,
                count(*) AS contexts,
                max(c.acceptance_datetime) AS latest_acceptance
            FROM xbrl_filing_contexts c
            LEFT JOIN sec_company_tickers t
              ON t.security_id = c.security_id
             AND t.cik = c.cik
            GROUP BY 1
            ORDER BY symbol
            """
        ).fetchall()
        summaries["xbrl_filing_contexts_by_symbol"] = [
            {
                "symbol": row[0],
                "filings": int(row[1]),
                "contexts": int(row[2]),
                "latest_acceptance": row[3],
            }
            for row in rows
        ]
    return summaries


def collect_job_definitions(store: DuckDBStore, job_names: list[str]) -> list[dict[str, Any]]:
    if not job_names:
        return []
    store.con.execute(
        "CREATE OR REPLACE TEMP TABLE bootstrap_job_names(job_name VARCHAR)"
    )
    store.con.executemany("INSERT INTO bootstrap_job_names VALUES (?)", [(name,) for name in job_names])
    rows = store.con.execute(
        """
        SELECT
            d.job_name,
            d.dataset_id,
            d.enabled,
            d.max_retries,
            d.retry_delay_seconds,
            d.dependencies_json,
            d.params_json
        FROM etl_job_definitions d
        JOIN bootstrap_job_names n USING (job_name)
        ORDER BY d.job_name
        """
    ).fetchall()
    return [
        {
            "job_name": row[0],
            "dataset_id": row[1],
            "enabled": row[2],
            "max_retries": row[3],
            "retry_delay_seconds": row[4],
            "dependencies_json": row[5],
            "params_json": row[6],
        }
        for row in rows
    ]


def run_registered_jobs(
    db_path: Path,
    args: argparse.Namespace,
    symbols: tuple[str, ...],
) -> dict[str, Any]:
    with DuckDBStore(db_path) as store:
        manager = JobManager(store)
        job_names = register_jobs(manager, args, symbols)
        definitions = collect_job_definitions(store, job_names)
        if args.register_only:
            return {"job_names": job_names, "definitions": definitions, "results": []}
        results = [dataset_result_payload(manager.run_job(name)) for name in job_names]
        return {"job_names": job_names, "definitions": definitions, "results": results}


def export_lake(db_path: Path, args: argparse.Namespace) -> dict[str, Any]:
    if args.skip_lake_export:
        return {"skipped": True}
    objects = comma_tuple(args.lake_objects) or DEFAULT_EXPORT_OBJECTS
    results = LakehouseExporter(db_path, args.lake_root).export_objects(objects)
    return {
        "skipped": False,
        "export_run_id": results[0].export_run_id if results else None,
        "object_count": len(results),
        "total_rows": sum(result.rows for result in results),
        "total_byte_count": sum(result.byte_count for result in results),
        "objects": [
            {
                "object_name": result.object_name,
                "rows": result.rows,
                "byte_count": result.byte_count,
                "sha256": result.sha256,
                "output_path": str(result.output_path),
                "manifest_path": str(result.manifest_path),
            }
            for result in results
        ],
    }


def refresh_watermarks(db_path: Path) -> dict[str, Any]:
    with DuckDBStore(db_path) as store:
        result = refresh_warehouse_watermarks(store)
    selected_names = {
        "tbltickerhistory_daily.min_trade_date",
        "tbltickerhistory_daily.max_trade_date",
        "equity_daily_features.max_as_of_date:equity_daily_v1",
        "sec_fundamental_features.max_as_of_date:sec_fundamentals_v1",
        "sec_submissions.max_filing_date",
        "xbrl_filing_contexts.max_acceptance_datetime",
        "xbrl_filing_contexts.max_source_loaded_at",
        "xbrl_filing_facts.max_acceptance_datetime",
        "xbrl_filing_facts.max_source_loaded_at",
        "alpha_research.max_as_of_date",
        "alpha_research.max_available_at",
        "alpha_research.max_backtest_end_date",
        "universe_memberships.max_as_of_date",
        "warehouse_lake_exports.last_succeeded_export",
    }
    selected = {
        f"{row['dataset_id']}.{row['watermark_name']}": row["watermark_value"]
        for row in result.watermarks
        if f"{row['dataset_id']}.{row['watermark_name']}" in selected_names
    }
    return {
        "rows_upserted": result.rows_upserted,
        "selected": selected,
    }


def run_quality(db_path: Path, args: argparse.Namespace) -> dict[str, Any]:
    if args.skip_quality:
        return {"skipped": True, "passed": 0, "warnings": 0, "failed": 0, "failures": []}
    with DuckDBStore(db_path) as store:
        results = run_warehouse_quality_checks(
            store,
            daily_macro_stale_days=args.daily_macro_stale_days,
            monthly_macro_stale_days=args.monthly_macro_stale_days,
            record=not args.no_record_quality,
        )
    failures = [result for result in results if result.status == "failed"]
    warnings = [result for result in results if result.status == "warning"]
    return {
        "skipped": False,
        "passed": len([result for result in results if result.status == "passed"]),
        "warnings": len(warnings),
        "failed": len(failures),
        "failures": [asdict(result) for result in failures],
        "warning_checks": [asdict(result) for result in warnings],
    }


def profile_archive(args: argparse.Namespace) -> dict[str, Any] | None:
    if args.skip_profile:
        return None
    return profile_ticker_history_archive(
        TickerHistoryArchiveProfileOptions(
            zip_path=args.ticker_history_zip,
            chunk_size=args.profile_chunk_size,
            max_chunks=args.profile_max_chunks,
            top_n=args.profile_top_n,
        )
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Bootstrap a bounded US equity research universe from the local "
            "tbltickerhistory archive, then refresh features, lake exports, "
            "watermarks, and quality checks."
        )
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--ticker-history-zip", type=Path, default=DEFAULT_TICKER_HISTORY_ZIP)
    parser.add_argument("--preset", choices=sorted(PRESETS), default="liquid40")
    parser.add_argument("--symbols", help="Comma/space-separated symbols. Overrides --preset when present.")
    parser.add_argument("--symbols-file", type=Path, help="One symbol per line, comments allowed with #.")
    parser.add_argument("--start-date", type=parse_date)
    parser.add_argument("--end-date", type=parse_date)

    parser.add_argument("--job-prefix", default="research")
    parser.add_argument("--max-retries", type=int, default=1)
    parser.add_argument("--retry-delay-seconds", type=float, default=5.0)
    parser.add_argument("--register-only", action="store_true")

    parser.add_argument("--ticker-max-chunks", type=int, default=8)
    parser.add_argument(
        "--full-ticker-history",
        action="store_true",
        help="Load the entire tbltickerhistory archive instead of the bounded --ticker-max-chunks slice.",
    )
    parser.add_argument("--ticker-chunk-size", type=int, default=200_000)
    parser.add_argument("--compute-source-hash", action="store_true")

    parser.add_argument("--skip-profile", action="store_true")
    parser.add_argument("--profile-max-chunks", type=int, default=2)
    parser.add_argument("--profile-chunk-size", type=int, default=200_000)
    parser.add_argument("--profile-top-n", type=int, default=10)

    parser.add_argument("--universe-id", default="us_liquid_equity_v1")
    parser.add_argument("--universe-name", default="US liquid equity research universe")
    parser.add_argument(
        "--universe-description",
        default="Trailing-liquidity PIT universe derived from canonical daily bars.",
    )
    parser.add_argument("--universe-lookback-days", type=int, default=20)
    parser.add_argument("--universe-min-history-days", type=int, default=20)
    parser.add_argument("--universe-min-price", type=float, default=5.0)
    parser.add_argument("--universe-min-dollar-volume", type=float, default=10_000_000.0)

    parser.add_argument("--feature-set", default="equity_daily_v1")
    parser.add_argument("--feature-min-rows", type=int, default=40)
    parser.add_argument("--include-sec-fundamentals", action="store_true")
    parser.add_argument("--fundamental-feature-set", default="sec_fundamentals_v1")
    parser.add_argument("--sec-company-facts-symbol-source", choices=COMPANY_FACT_SYMBOL_SOURCES, default="symbols")
    parser.add_argument("--sec-company-facts-symbol-limit", type=int)
    parser.add_argument("--sec-company-facts-universe-id")
    parser.add_argument("--sec-company-facts-as-of-date", type=parse_date)
    parser.add_argument("--xbrl-taxonomy-urls", default=",".join(DEFAULT_XBRL_TAXONOMY_PACKAGE_URLS))
    parser.add_argument("--xbrl-request-timeout", type=int, default=120)
    parser.add_argument("--xbrl-user-agent", default="atx-impl XBRL taxonomy research bootstrap nathan.tormaschy@gmail.com")
    parser.add_argument("--include-sec-submissions", action="store_true")
    parser.add_argument("--sec-forms", default="10-K,10-Q,8-K")
    parser.add_argument("--include-xbrl-filing-contexts", action="store_true")
    parser.add_argument("--xbrl-context-forms", default="10-K,10-Q")
    parser.add_argument("--xbrl-context-max-filings", type=int, default=3)
    parser.add_argument(
        "--xbrl-context-max-filings-per-symbol",
        type=int,
        help="Optional balanced cap per security before the overall --xbrl-context-max-filings cap.",
    )
    parser.add_argument("--xbrl-context-request-timeout", type=int, default=120)
    parser.add_argument("--xbrl-context-user-agent", default="atx-impl XBRL filing context research bootstrap nathan.tormaschy@gmail.com")

    parser.add_argument("--skip-lake-export", action="store_true")
    parser.add_argument("--lake-root", type=Path, default=DEFAULT_LAKE_ROOT)
    parser.add_argument("--lake-objects", help="Comma-separated table/view list. Defaults to curated lake objects.")

    parser.add_argument("--skip-quality", action="store_true")
    parser.add_argument("--no-record-quality", action="store_true")
    parser.add_argument("--allow-quality-failures", action="store_true")
    parser.add_argument("--daily-macro-stale-days", type=int, default=10)
    parser.add_argument("--monthly-macro-stale-days", type=int, default=70)
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    if args.start_date and args.end_date and args.start_date > args.end_date:
        raise ValueError("--start-date cannot be after --end-date")
    if args.max_retries < 0:
        raise ValueError("--max-retries must be >= 0")
    if args.retry_delay_seconds < 0:
        raise ValueError("--retry-delay-seconds must be >= 0")
    if args.ticker_chunk_size < 1:
        raise ValueError("--ticker-chunk-size must be positive")
    if not args.full_ticker_history and args.ticker_max_chunks < 1:
        raise ValueError("--ticker-max-chunks must be positive unless --full-ticker-history is set")
    if args.profile_chunk_size < 1:
        raise ValueError("--profile-chunk-size must be positive")
    if args.profile_max_chunks is not None and args.profile_max_chunks < 1:
        raise ValueError("--profile-max-chunks must be positive")
    if args.profile_top_n < 1:
        raise ValueError("--profile-top-n must be positive")
    if args.universe_lookback_days < 1:
        raise ValueError("--universe-lookback-days must be positive")
    if args.universe_min_history_days < 1:
        raise ValueError("--universe-min-history-days must be positive")
    if args.universe_min_history_days > args.universe_lookback_days:
        raise ValueError("--universe-min-history-days cannot exceed --universe-lookback-days")
    if args.feature_min_rows < 1:
        raise ValueError("--feature-min-rows must be positive")
    if args.xbrl_request_timeout < 1:
        raise ValueError("--xbrl-request-timeout must be positive")
    if args.xbrl_context_request_timeout < 1:
        raise ValueError("--xbrl-context-request-timeout must be positive")
    if args.xbrl_context_max_filings < 1:
        raise ValueError("--xbrl-context-max-filings must be positive")
    if args.xbrl_context_max_filings_per_symbol is not None and args.xbrl_context_max_filings_per_symbol < 1:
        raise ValueError("--xbrl-context-max-filings-per-symbol must be positive")
    if args.include_xbrl_filing_contexts and not args.include_sec_submissions:
        raise ValueError("--include-xbrl-filing-contexts requires --include-sec-submissions")
    if args.sec_company_facts_symbol_limit is not None and args.sec_company_facts_symbol_limit < 1:
        raise ValueError("--sec-company-facts-symbol-limit must be positive")
    if args.include_sec_fundamentals and not comma_tuple(args.xbrl_taxonomy_urls):
        raise ValueError("--xbrl-taxonomy-urls must include at least one URL when --include-sec-fundamentals is set")
    if args.daily_macro_stale_days < 1 or args.monthly_macro_stale_days < 1:
        raise ValueError("Macro staleness thresholds must be positive")


def main() -> int:
    args = parse_args()
    validate_args(args)
    symbols = resolve_symbols(args)
    if not symbols:
        raise ValueError("No symbols resolved for bootstrap")
    if not args.ticker_history_zip.exists():
        raise FileNotFoundError(args.ticker_history_zip)

    profile = profile_archive(args)
    jobs = run_registered_jobs(args.db_path, args, symbols)
    lake_export = {"skipped": True}
    watermarks = {"rows_upserted": 0, "selected": {}}
    quality = {"skipped": True, "passed": 0, "warnings": 0, "failed": 0, "failures": []}
    row_counts: dict[str, int] = {}
    curated_summaries: dict[str, Any] = {}

    if not args.register_only:
        lake_export = export_lake(args.db_path, args)
        watermarks = refresh_watermarks(args.db_path)
        quality = run_quality(args.db_path, args)
        with DuckDBStore(args.db_path, read_only=True) as store:
            row_counts = collect_row_counts(store)
            curated_summaries = collect_curated_summaries(store)

    output = {
        "db_path": str(args.db_path),
        "ticker_history_zip": str(args.ticker_history_zip),
        "symbols": list(symbols),
        "symbol_count": len(symbols),
        "ticker_max_chunks": effective_ticker_max_chunks(args),
        "ticker_chunk_size": args.ticker_chunk_size,
        "profile": profile,
        "jobs": jobs,
        "lake_export": lake_export,
        "watermarks": watermarks,
        "quality": quality,
        "row_counts": row_counts,
        "curated_summaries": curated_summaries,
    }
    print(json.dumps(output, indent=2, default=str))
    if quality.get("failed", 0) and not args.allow_quality_failures:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
