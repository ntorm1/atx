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

from db import DEFAULT_DB_PATH, DuckDBStore, JobManager, validate_lake_export
from db.lake import DEFAULT_EXPORT_OBJECTS, DEFAULT_LAKE_ROOT, LakehouseExporter
from db.fundamentals import COMPANY_FACT_SYMBOL_SOURCES
from db.macro import DEFAULT_SERIES
from db.quality import run_warehouse_quality_checks
from db.thirteenf import AAPL_CUSIP, ThirteenFOptions, normalize_cusip
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

PUBLIC_ROW_COUNT_TABLES = (
    "sec_company_tickers",
    "nasdaq_symbol_directory",
    "nasdaq_listing_events",
    "listing_status_intervals",
    "delist_code_dim",
    "delisting_events",
    "sec_company_facts",
    "fundamental_points",
    "xbrl_taxonomy_packages",
    "xbrl_taxonomy_roles",
    "xbrl_taxonomy_relationships",
    "xbrl_dimension_edges",
    "xbrl_fact_frames",
    "xbrl_filing_contexts",
    "xbrl_filing_dimensions",
    "xbrl_filing_facts",
    "fundamental_fact_revisions",
    "fundamental_statement_map",
    "fundamental_statement_points",
    "fundamental_ttm_points",
    "fundamental_periods",
    "sec_submissions",
    "thirteenf_submissions",
    "thirteenf_cover_pages",
    "thirteenf_summary_pages",
    "thirteenf_holdings",
    "thirteenf_managers",
    "thirteenf_manager_reports",
    "thirteenf_security_positions",
    "thirteenf_security_ownership",
    "finra_short_interest",
    "finra_short_interest_backfill_manifests",
    "macro_series",
    "macro_observations",
    "identifier_resolution_candidates",
    "identifier_resolution_decisions",
    "security_identifier_history",
    "exchange_listings",
    "feature_values",
    "feature_set_catalog",
    "feature_dependency_edges",
    "feature_build_manifests",
    "alpha_expression_catalog",
    "alpha_signal_values",
    "alpha_backtest_manifests",
    "raw_source_files",
    "dataset_watermarks",
    "dataset_runs",
    "etl_job_definitions",
    "etl_job_runs",
    "etl_job_events",
    "data_quality_checks",
    "lake_export_runs",
    "lake_export_files",
)


def parse_date(value: str | None) -> dt.date | None:
    if value in (None, ""):
        return None
    return dt.date.fromisoformat(value)


def split_csv(value: str | None) -> tuple[str, ...]:
    if not value:
        return ()
    return tuple(part.strip() for part in value.split(",") if part.strip())


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


def resolve_symbols(args: argparse.Namespace) -> tuple[str, ...]:
    explicit = [*parse_symbols_text(args.symbols), *load_symbols_file(args.symbols_file)]
    if explicit:
        return tuple(sorted(set(explicit)))
    return tuple(sorted(set(PRESETS[args.preset])))


def normalize_cusips_arg(value: str | None) -> tuple[str, ...]:
    cusips = split_csv(value)
    return tuple(normalize_cusip(cusip) for cusip in cusips if normalize_cusip(cusip))


def clean_params(params: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in params.items() if value is not None}


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


def register_public_jobs(manager: JobManager, args: argparse.Namespace, symbols: tuple[str, ...]) -> list[str]:
    retry_policy = {
        "max_retries": args.max_retries,
        "retry_delay_seconds": args.retry_delay_seconds,
    }
    names = {
        "security_master": job_name(args.job_prefix, "security_master"),
        "nasdaq_symbol_directory": job_name(args.job_prefix, "nasdaq_symbol_directory"),
        "nasdaq_listing_events": job_name(args.job_prefix, "nasdaq_listing_events"),
        "listing_status_intervals": job_name(args.job_prefix, "listing_status_intervals"),
        "delisting_events": job_name(args.job_prefix, "delisting_events"),
        "sec_company_facts": job_name(args.job_prefix, "sec_company_facts"),
        "xbrl_taxonomy": job_name(args.job_prefix, "xbrl_taxonomy"),
        "sec_fundamental_features": job_name(args.job_prefix, "sec_fundamental_features"),
        "sec_submissions": job_name(args.job_prefix, "sec_submissions"),
        "xbrl_filing_contexts": job_name(args.job_prefix, "xbrl_filing_contexts"),
        "finra_short_interest": job_name(args.job_prefix, "finra_short_interest"),
        "finra_short_interest_features": job_name(args.job_prefix, "finra_short_interest_features"),
        "sec_13f": job_name(args.job_prefix, "sec_13f"),
        "sec_13f_ownership_features": job_name(args.job_prefix, "sec_13f_ownership_features"),
        "identifier_resolution_candidates": job_name(args.job_prefix, "identifier_resolution_candidates"),
        "identifier_resolution_decisions": job_name(args.job_prefix, "identifier_resolution_decisions"),
        "fred_macro": job_name(args.job_prefix, "fred_macro"),
    }
    ordered: list[str] = []

    if not args.skip_security_master:
        manager.register_job(
            job_name=names["security_master"],
            dataset_id="sec_security_master",
            params=clean_params(
                {
                    "request_timeout": args.sec_request_timeout,
                    "user_agent": args.sec_user_agent,
                }
            ),
            **retry_policy,
        )
        ordered.append(names["security_master"])

    if not args.skip_symbol_directory:
        manager.register_job(
            job_name=names["nasdaq_symbol_directory"],
            dataset_id="nasdaq_symbol_directory",
            params=clean_params(
                {
                    "as_of_date": args.symbol_directory_as_of_date,
                    "request_timeout": args.nasdaq_request_timeout,
                    "user_agent": args.nasdaq_user_agent,
                }
            ),
            **retry_policy,
        )
        ordered.append(names["nasdaq_symbol_directory"])

    security_dependency = [] if args.skip_security_master else [names["security_master"]]

    if not args.skip_listing_events:
        manager.register_job(
            job_name=names["nasdaq_listing_events"],
            dataset_id="nasdaq_listing_events",
            params=clean_params(
                {
                    "as_of_date": args.listing_events_as_of_date,
                    "source_url": args.listing_events_url,
                    "request_timeout": args.nasdaq_request_timeout,
                    "user_agent": args.nasdaq_user_agent,
                }
            ),
            dependencies=security_dependency,
            **retry_policy,
        )
        ordered.append(names["nasdaq_listing_events"])

    listing_status_dependencies = [
        name
        for skip, name in (
            (args.skip_security_master, names["security_master"]),
            (args.skip_symbol_directory, names["nasdaq_symbol_directory"]),
            (args.skip_listing_events, names["nasdaq_listing_events"]),
        )
        if not skip
    ]
    if not args.skip_listing_status:
        manager.register_job(
            job_name=names["listing_status_intervals"],
            dataset_id="listing_status_intervals",
            params={},
            dependencies=listing_status_dependencies,
            **retry_policy,
        )
        ordered.append(names["listing_status_intervals"])

    if not args.skip_delistings:
        manager.register_job(
            job_name=names["delisting_events"],
            dataset_id="delisting_events",
            params=clean_params(
                {
                    "apply_shumway_warther_imputation": args.impute_delist_returns,
                }
            ),
            dependencies=[] if args.skip_listing_status else [names["listing_status_intervals"]],
            **retry_policy,
        )
        ordered.append(names["delisting_events"])

    if not args.skip_company_facts:
        manager.register_job(
            job_name=names["sec_company_facts"],
            dataset_id="sec_company_facts",
            params=clean_params(
                {
                    "symbols": symbols,
                    "concepts": split_csv(args.sec_concepts),
                    "symbol_source": args.sec_company_facts_symbol_source,
                    "symbol_limit": args.sec_company_facts_symbol_limit,
                    "universe_id": args.sec_company_facts_universe_id,
                    "as_of_date": args.sec_company_facts_as_of_date,
                    "request_timeout": args.sec_request_timeout,
                    "user_agent": args.sec_user_agent,
                }
            ),
            dependencies=security_dependency,
            **retry_policy,
        )
        ordered.append(names["sec_company_facts"])

    if not args.skip_xbrl_taxonomy:
        manager.register_job(
            job_name=names["xbrl_taxonomy"],
            dataset_id="xbrl_taxonomy",
            params=clean_params(
                {
                    "package_urls": split_csv(args.xbrl_taxonomy_urls),
                    "request_timeout": args.xbrl_request_timeout,
                    "user_agent": args.xbrl_user_agent,
                }
            ),
            dependencies=[] if args.skip_company_facts else [names["sec_company_facts"]],
            **retry_policy,
        )
        ordered.append(names["xbrl_taxonomy"])

    if not args.skip_fundamental_features:
        manager.register_job(
            job_name=names["sec_fundamental_features"],
            dataset_id="sec_fundamental_features",
            params=clean_params(
                {
                    "symbols": symbols,
                    "feature_set": args.fundamental_feature_set,
                    "start_date": args.fundamental_feature_start_date,
                    "end_date": args.fundamental_feature_end_date,
                }
            ),
            dependencies=[] if args.skip_company_facts else [names["sec_company_facts"]],
            **retry_policy,
        )
        ordered.append(names["sec_fundamental_features"])

    if not args.skip_submissions:
        manager.register_job(
            job_name=names["sec_submissions"],
            dataset_id="sec_submissions",
            params=clean_params(
                {
                    "symbols": symbols,
                    "forms": None if args.sec_forms.lower() == "all" else split_csv(args.sec_forms),
                    "include_history_files": args.include_submission_history,
                    "request_timeout": args.sec_request_timeout,
                    "user_agent": args.sec_user_agent,
                }
            ),
            dependencies=security_dependency,
            **retry_policy,
        )
        ordered.append(names["sec_submissions"])

    if not args.skip_xbrl_filing_contexts:
        manager.register_job(
            job_name=names["xbrl_filing_contexts"],
            dataset_id="xbrl_filing_contexts",
            params=clean_params(
                {
                    "symbols": symbols,
                    "forms": split_csv(args.xbrl_context_forms),
                    "max_filings": args.xbrl_context_max_filings,
                    "max_filings_per_symbol": args.xbrl_context_max_filings_per_symbol,
                    "request_timeout": args.xbrl_context_request_timeout,
                    "user_agent": args.xbrl_context_user_agent,
                }
            ),
            dependencies=[] if args.skip_submissions else [names["sec_submissions"]],
            **retry_policy,
        )
        ordered.append(names["xbrl_filing_contexts"])

    if not args.skip_finra:
        finra_symbol = None if args.finra_mode == "dates" else symbol_key(args.finra_symbol or symbols[0])
        manager.register_job(
            job_name=names["finra_short_interest"],
            dataset_id="finra_short_interest",
            params=clean_params(
                {
                    "symbol": finra_symbol,
                    "start_date": args.finra_start_date,
                    "end_date": args.finra_end_date,
                    "limit": args.finra_limit,
                    "limit_dates": args.finra_limit_dates,
                    "date_order": args.finra_date_order,
                    "request_timeout": args.finra_request_timeout,
                    "max_retries": args.finra_request_retries,
                    "retry_sleep": args.finra_retry_sleep,
                    "user_agent": args.finra_user_agent,
                }
            ),
            dependencies=security_dependency,
            **retry_policy,
        )
        ordered.append(names["finra_short_interest"])

    if not args.skip_finra_features:
        manager.register_job(
            job_name=names["finra_short_interest_features"],
            dataset_id="finra_short_interest_features",
            params=clean_params(
                {
                    "feature_set": args.finra_feature_set,
                    "min_cross_section": args.finra_feature_min_cross_section,
                }
            ),
            dependencies=[] if args.skip_finra else [names["finra_short_interest"]],
            **retry_policy,
        )
        ordered.append(names["finra_short_interest_features"])

    if not args.skip_13f:
        cusips = None if args.full_13f_holdings else normalize_cusips_arg(args.cusips)
        manager.register_job(
            job_name=names["sec_13f"],
            dataset_id="sec_13f",
            params=clean_params(
                {
                    "dataset_url": args.sec_13f_dataset_url,
                    "cache_dir": str(args.sec_13f_cache_dir),
                    "cusips": cusips,
                    "full_holdings": args.full_13f_holdings,
                    "chunk_size": args.sec_13f_chunk_size,
                    "request_timeout": args.sec_13f_request_timeout,
                    "user_agent": args.sec_13f_user_agent,
                    "compute_source_hash": args.compute_source_hash,
                }
            ),
            dependencies=security_dependency,
            **retry_policy,
        )
        ordered.append(names["sec_13f"])

    if not args.skip_13f_ownership_features:
        manager.register_job(
            job_name=names["sec_13f_ownership_features"],
            dataset_id="sec_13f_ownership_features",
            params=clean_params(
                {
                    "feature_set": args.sec_13f_ownership_feature_set,
                }
            ),
            dependencies=[] if args.skip_13f else [names["sec_13f"]],
            **retry_policy,
        )
        ordered.append(names["sec_13f_ownership_features"])

    if not args.skip_identifier_resolution:
        manager.register_job(
            job_name=names["identifier_resolution_candidates"],
            dataset_id="identifier_resolution_candidates",
            params=clean_params(
                {
                    "source_dataset_id": args.identifier_source_dataset_id,
                    "source_period": args.identifier_source_period,
                    "min_confidence": args.identifier_min_confidence,
                    "include_already_mapped": args.identifier_include_already_mapped,
                }
            ),
            dependencies=[] if args.skip_13f else [names["sec_13f"]],
            **retry_policy,
        )
        ordered.append(names["identifier_resolution_candidates"])

    if not args.skip_identifier_decisions:
        manager.register_job(
            job_name=names["identifier_resolution_decisions"],
            dataset_id="identifier_resolution_decisions",
            params=clean_params(
                {
                    "source_dataset_id": args.identifier_source_dataset_id,
                    "source_period": args.identifier_source_period,
                    "min_accept_confidence": args.identifier_min_accept_confidence,
                    "min_review_confidence": args.identifier_min_review_confidence,
                    "accept_candidate_statuses": split_csv(args.identifier_accept_statuses),
                    "review_candidate_statuses": split_csv(args.identifier_review_statuses),
                    "apply_accepted": not args.do_not_apply_identifier_decisions,
                    "decision_method": args.identifier_decision_method,
                    "decided_by": args.identifier_decided_by,
                }
            ),
            dependencies=[] if args.skip_identifier_resolution else [names["identifier_resolution_candidates"]],
            **retry_policy,
        )
        ordered.append(names["identifier_resolution_decisions"])

    if not args.skip_macro:
        manager.register_job(
            job_name=names["fred_macro"],
            dataset_id="fred_macro",
            params=clean_params(
                {
                    "series_ids": split_csv(args.macro_series),
                    "start_date": args.macro_start_date,
                    "end_date": args.macro_end_date,
                    "request_timeout": args.macro_request_timeout,
                    "user_agent": args.macro_user_agent,
                }
            ),
            **retry_policy,
        )
        ordered.append(names["fred_macro"])

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


def collect_row_counts(store: DuckDBStore, tables: tuple[str, ...] = PUBLIC_ROW_COUNT_TABLES) -> dict[str, int]:
    counts: dict[str, int] = {}
    for table in tables:
        if table_or_view_exists(store, table):
            quoted = '"' + table.replace('"', '""') + '"'
            counts[table] = int(store.con.execute(f"SELECT count(*) FROM {quoted}").fetchone()[0])
    return counts


def collect_job_definitions(store: DuckDBStore, job_names: list[str]) -> list[dict[str, Any]]:
    if not job_names:
        return []
    store.con.execute("CREATE OR REPLACE TEMP TABLE public_bootstrap_job_names(job_name VARCHAR)")
    store.con.executemany("INSERT INTO public_bootstrap_job_names VALUES (?)", [(name,) for name in job_names])
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
        JOIN public_bootstrap_job_names n USING (job_name)
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


def collect_public_summaries(store: DuckDBStore) -> dict[str, Any]:
    summaries: dict[str, Any] = {}
    if table_or_view_exists(store, "sec_company_tickers"):
        row = store.con.execute(
            """
            SELECT count(*) AS rows, count(DISTINCT ticker) AS tickers, count(DISTINCT cik) AS ciks
            FROM sec_company_tickers
            """
        ).fetchone()
        summaries["sec_company_tickers"] = {"rows": int(row[0]), "tickers": int(row[1]), "ciks": int(row[2])}
    if table_or_view_exists(store, "nasdaq_symbol_directory"):
        row = store.con.execute(
            """
            SELECT
                count(*) AS rows,
                count(DISTINCT symbol) AS symbols,
                min(as_of_date) AS min_as_of_date,
                max(as_of_date) AS max_as_of_date
            FROM nasdaq_symbol_directory
            """
        ).fetchone()
        summaries["nasdaq_symbol_directory"] = {
            "rows": int(row[0]),
            "symbols": int(row[1]),
            "min_as_of_date": row[2],
            "max_as_of_date": row[3],
        }
    if table_or_view_exists(store, "nasdaq_listing_events"):
        row = store.con.execute(
            """
            SELECT
                count(*) AS rows,
                count(DISTINCT symbol) AS symbols,
                count(security_id) AS resolved_security_ids,
                min(effective_date) AS min_effective_date,
                max(effective_date) AS max_effective_date,
                max(source_file_created_at) AS max_source_file_created_at
            FROM nasdaq_listing_events
            """
        ).fetchone()
        summaries["nasdaq_listing_events"] = {
            "rows": int(row[0]),
            "symbols": int(row[1]),
            "resolved_security_ids": int(row[2]),
            "min_effective_date": row[3],
            "max_effective_date": row[4],
            "max_source_file_created_at": row[5],
        }
    if table_or_view_exists(store, "listing_status_intervals"):
        row = store.con.execute(
            """
            SELECT
                count(*) AS rows,
                count(DISTINCT symbol) AS symbols,
                count(security_id) AS resolved_security_rows,
                sum(CASE WHEN status = 'active' THEN 1 ELSE 0 END) AS active_rows,
                sum(CASE WHEN status = 'inactive' THEN 1 ELSE 0 END) AS inactive_rows,
                min(valid_from) AS min_valid_from,
                max(valid_from) AS max_valid_from,
                max(available_at) AS max_available_at
            FROM listing_status_intervals
            """
        ).fetchone()
        summaries["listing_status_intervals"] = {
            "rows": int(row[0]),
            "symbols": int(row[1]),
            "resolved_security_rows": int(row[2]),
            "active_rows": int(row[3] or 0),
            "inactive_rows": int(row[4] or 0),
            "min_valid_from": row[5],
            "max_valid_from": row[6],
            "max_available_at": row[7],
        }
    if table_or_view_exists(store, "delisting_events"):
        row = store.con.execute(
            """
            SELECT
                count(*) AS rows,
                count(DISTINCT symbol) AS symbols,
                count(security_id) AS resolved_security_rows,
                sum(CASE WHEN is_return_imputed THEN 1 ELSE 0 END) AS imputed_return_rows,
                min(delist_date) AS min_delist_date,
                max(delist_date) AS max_delist_date,
                max(available_at) AS max_available_at
            FROM delisting_events
            """
        ).fetchone()
        summaries["delisting_events"] = {
            "rows": int(row[0]),
            "symbols": int(row[1]),
            "resolved_security_rows": int(row[2]),
            "imputed_return_rows": int(row[3] or 0),
            "min_delist_date": row[4],
            "max_delist_date": row[5],
            "max_available_at": row[6],
        }
    if table_or_view_exists(store, "fundamental_points"):
        rows = store.con.execute(
            """
            SELECT symbol, count(*) AS rows, min(as_of_date) AS min_as_of_date, max(as_of_date) AS max_as_of_date
            FROM fundamental_points
            GROUP BY 1
            ORDER BY rows DESC, symbol
            LIMIT 20
            """
        ).fetchall()
        summaries["fundamental_points_top_symbols"] = [
            {"symbol": row[0], "rows": int(row[1]), "min_as_of_date": row[2], "max_as_of_date": row[3]}
            for row in rows
        ]
    if table_or_view_exists(store, "sec_submissions"):
        rows = store.con.execute(
            """
            SELECT form, count(*) AS rows, max(filing_date) AS max_filing_date
            FROM sec_submissions
            GROUP BY 1
            ORDER BY rows DESC, form
            """
        ).fetchall()
        summaries["sec_submissions_by_form"] = [
            {"form": row[0], "rows": int(row[1]), "max_filing_date": row[2]} for row in rows
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
    if table_or_view_exists(store, "thirteenf_holdings"):
        rows = store.con.execute(
            """
            SELECT source_period, count(*) AS holdings, count(DISTINCT cusip) AS cusips
            FROM thirteenf_holdings
            GROUP BY 1
            ORDER BY source_period DESC
            LIMIT 10
            """
        ).fetchall()
        summaries["thirteenf_periods"] = [
            {"source_period": row[0], "holdings": int(row[1]), "cusips": int(row[2])} for row in rows
        ]
    if table_or_view_exists(store, "thirteenf_security_ownership"):
        rows = store.con.execute(
            """
            SELECT
                source_period,
                count(*) AS ownership_rows,
                count(DISTINCT security_id) AS securities,
                count(DISTINCT cusip) AS cusips,
                sum(common_value_usd) AS common_value_usd,
                max(available_at) AS max_available_at
            FROM thirteenf_security_ownership
            GROUP BY 1
            ORDER BY source_period DESC
            LIMIT 10
            """
        ).fetchall()
        summaries["thirteenf_ownership_periods"] = [
            {
                "source_period": row[0],
                "ownership_rows": int(row[1]),
                "securities": int(row[2]),
                "cusips": int(row[3]),
                "common_value_usd": row[4],
                "max_available_at": row[5],
            }
            for row in rows
        ]
    if table_or_view_exists(store, "finra_short_interest"):
        rows = store.con.execute(
            """
            SELECT symbol, count(*) AS rows, min(settlement_date) AS min_settlement_date, max(settlement_date) AS max_settlement_date
            FROM finra_short_interest
            GROUP BY 1
            ORDER BY rows DESC, symbol
            LIMIT 20
            """
        ).fetchall()
        summaries["finra_short_interest_symbols"] = [
            {
                "symbol": row[0],
                "rows": int(row[1]),
                "min_settlement_date": row[2],
                "max_settlement_date": row[3],
            }
            for row in rows
        ]
    if table_or_view_exists(store, "finra_short_interest_backfill_manifests"):
        rows = store.con.execute(
            """
            SELECT
                manifest_id,
                status,
                start_date,
                end_date,
                selected_date_count,
                loaded_date_count,
                source_row_count,
                feature_row_count,
                finished_at
            FROM finra_short_interest_backfill_manifests
            ORDER BY finished_at DESC
            LIMIT 10
            """
        ).fetchall()
        summaries["finra_short_interest_backfills"] = [
            {
                "manifest_id": row[0],
                "status": row[1],
                "start_date": row[2],
                "end_date": row[3],
                "selected_date_count": int(row[4]),
                "loaded_date_count": int(row[5]),
                "source_row_count": int(row[6]),
                "feature_row_count": None if row[7] is None else int(row[7]),
                "finished_at": row[8],
            }
            for row in rows
        ]
    if table_or_view_exists(store, "feature_values"):
        rows = store.con.execute(
            """
            SELECT
                feature_name,
                count(*) AS rows,
                min(as_of_date) AS min_as_of_date,
                max(as_of_date) AS max_as_of_date,
                max(available_at) AS max_available_at
            FROM feature_values
            WHERE feature_set = 'finra_short_interest_v1'
            GROUP BY 1
            ORDER BY 1
            """
        ).fetchall()
        if rows:
            summaries["finra_short_interest_features"] = [
                {
                    "feature_name": row[0],
                    "rows": int(row[1]),
                    "min_as_of_date": row[2],
                    "max_as_of_date": row[3],
                    "max_available_at": row[4],
                }
                for row in rows
            ]
    if table_or_view_exists(store, "macro_observations"):
        rows = store.con.execute(
            """
            SELECT series_id, count(*) AS rows, max(observation_date) AS max_observation_date
            FROM macro_observations
            GROUP BY 1
            ORDER BY 1
            """
        ).fetchall()
        summaries["macro_series"] = [
            {"series_id": row[0], "rows": int(row[1]), "max_observation_date": row[2]} for row in rows
        ]
    return summaries


def run_registered_jobs(db_path: Path, args: argparse.Namespace, symbols: tuple[str, ...]) -> dict[str, Any]:
    with DuckDBStore(db_path) as store:
        manager = JobManager(store)
        job_names = register_public_jobs(manager, args, symbols)
        definitions = collect_job_definitions(store, job_names)
        if args.register_only:
            return {"job_names": job_names, "definitions": definitions, "results": []}
        results = [dataset_result_payload(manager.run_job(name)) for name in job_names]
        return {"job_names": job_names, "definitions": definitions, "results": results}


def export_lake(db_path: Path, args: argparse.Namespace) -> dict[str, Any]:
    if args.skip_lake_export:
        return {"skipped": True}
    objects = split_csv(args.lake_objects) or DEFAULT_EXPORT_OBJECTS
    results = LakehouseExporter(db_path, args.lake_root).export_objects(objects)
    return {
        "skipped": False,
        "export_run_id": results[0].export_run_id if results else None,
        "object_count": len(results),
        "total_rows": sum(result.rows for result in results),
        "total_byte_count": sum(result.byte_count for result in results),
    }


def validate_lake(db_path: Path, args: argparse.Namespace, lake_export: dict[str, Any]) -> dict[str, Any]:
    if args.skip_lake_validation:
        return {"skipped": True, "problem_count": 0}
    export_run_id = lake_export.get("export_run_id") if not lake_export.get("skipped") else None
    summary = validate_lake_export(db_path, export_run_id=export_run_id)
    return asdict(summary) | {"skipped": False, "problem_count": len(summary.problems)}


def refresh_watermarks(db_path: Path) -> dict[str, Any]:
    with DuckDBStore(db_path) as store:
        result = refresh_warehouse_watermarks(store)
    selected_prefixes = {
        "finra_short_interest.",
        "fred_macro.",
        "identifier_resolution_candidates.",
        "identifier_resolution_decisions.",
        "delist_code_dim.",
        "delisting_events.",
        "listing_status_intervals.",
        "nasdaq_listing_events.",
        "nasdaq_symbol_directory.",
        "finra_short_interest_features.",
        "sec_13f_ownership_features.",
        "sec_13f.",
        "sec_company_facts.",
        "xbrl_taxonomy.",
        "xbrl_dimensions.",
        "xbrl_fact_frames.",
        "xbrl_filing_contexts.",
        "xbrl_filing_facts.",
        "sec_fundamental_features.",
        "sec_submissions.",
        "alpha_research.",
        "warehouse_lake_exports.",
    }
    selected = {
        f"{row['dataset_id']}.{row['watermark_name']}": row["watermark_value"]
        for row in result.watermarks
        if any(f"{row['dataset_id']}.".startswith(prefix) for prefix in selected_prefixes)
    }
    return {"rows_upserted": result.rows_upserted, "selected": selected}


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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Bootstrap public-source warehouse feeds with ETL job runs, watermarks, quality checks, and lake validation."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--preset", choices=sorted(PRESETS), default="aapl")
    parser.add_argument("--symbols", help="Comma/space-separated symbols. Overrides --preset when present.")
    parser.add_argument("--symbols-file", type=Path)
    parser.add_argument("--job-prefix", default="public")
    parser.add_argument("--max-retries", type=int, default=1)
    parser.add_argument("--retry-delay-seconds", type=float, default=10.0)
    parser.add_argument("--register-only", action="store_true")

    parser.add_argument("--skip-security-master", action="store_true")
    parser.add_argument("--skip-symbol-directory", action="store_true")
    parser.add_argument("--skip-listing-events", action="store_true")
    parser.add_argument("--skip-listing-status", action="store_true")
    parser.add_argument("--skip-delistings", action="store_true")
    parser.add_argument("--impute-delist-returns", action="store_true")
    parser.add_argument("--skip-company-facts", action="store_true")
    parser.add_argument("--skip-xbrl-taxonomy", action="store_true")
    parser.add_argument("--skip-fundamental-features", action="store_true")
    parser.add_argument("--skip-submissions", action="store_true")
    parser.add_argument("--skip-xbrl-filing-contexts", action="store_true")
    parser.add_argument("--skip-finra", action="store_true")
    parser.add_argument("--skip-finra-features", action="store_true")
    parser.add_argument("--skip-13f", action="store_true")
    parser.add_argument("--skip-13f-ownership-features", action="store_true")
    parser.add_argument("--skip-identifier-resolution", action="store_true")
    parser.add_argument("--skip-identifier-decisions", action="store_true")
    parser.add_argument("--skip-macro", action="store_true")

    parser.add_argument("--sec-request-timeout", type=int, default=120)
    parser.add_argument("--sec-user-agent", default="atx-impl public-source bootstrap nathan.tormaschy@gmail.com")
    parser.add_argument("--sec-concepts", default="")
    parser.add_argument("--sec-company-facts-symbol-source", choices=COMPANY_FACT_SYMBOL_SOURCES, default="symbols")
    parser.add_argument("--sec-company-facts-symbol-limit", type=int)
    parser.add_argument("--sec-company-facts-universe-id")
    parser.add_argument("--sec-company-facts-as-of-date", type=parse_date)
    parser.add_argument("--sec-forms", default="10-K,10-Q,8-K", help="Comma-separated forms, or 'all'.")
    parser.add_argument("--include-submission-history", action="store_true")
    parser.add_argument("--xbrl-context-forms", default="10-K,10-Q")
    parser.add_argument("--xbrl-context-max-filings", type=int, default=3)
    parser.add_argument(
        "--xbrl-context-max-filings-per-symbol",
        type=int,
        help="Optional balanced cap per security before the overall --xbrl-context-max-filings cap.",
    )
    parser.add_argument("--xbrl-context-request-timeout", type=int, default=120)
    parser.add_argument("--xbrl-context-user-agent", default="atx-impl XBRL filing context bootstrap nathan.tormaschy@gmail.com")
    parser.add_argument("--xbrl-taxonomy-urls", default=",".join(DEFAULT_XBRL_TAXONOMY_PACKAGE_URLS))
    parser.add_argument("--xbrl-request-timeout", type=int, default=120)
    parser.add_argument("--xbrl-user-agent", default="atx-impl XBRL taxonomy bootstrap nathan.tormaschy@gmail.com")
    parser.add_argument("--fundamental-feature-set", default="sec_fundamentals_v1")
    parser.add_argument("--fundamental-feature-start-date", type=parse_date)
    parser.add_argument("--fundamental-feature-end-date", type=parse_date)

    parser.add_argument("--symbol-directory-as-of-date", type=parse_date)
    parser.add_argument("--listing-events-as-of-date", type=parse_date)
    parser.add_argument("--listing-events-url")
    parser.add_argument("--nasdaq-request-timeout", type=int, default=60)
    parser.add_argument("--nasdaq-user-agent", default="atx-impl symbol directory nathan.tormaschy@gmail.com")

    parser.add_argument("--finra-mode", choices=("symbol", "dates"), default="symbol")
    parser.add_argument("--finra-symbol")
    parser.add_argument("--finra-start-date", type=parse_date)
    parser.add_argument("--finra-end-date", type=parse_date)
    parser.add_argument("--finra-limit", type=int, default=5000)
    parser.add_argument("--finra-limit-dates", type=int)
    parser.add_argument(
        "--finra-date-order",
        choices=("asc", "desc"),
        default="desc",
        help="Settlement-date fetch order for --finra-mode dates; desc with --finra-limit-dates refreshes newest dates.",
    )
    parser.add_argument("--finra-request-timeout", type=int, default=120)
    parser.add_argument("--finra-request-retries", type=int, default=5)
    parser.add_argument("--finra-retry-sleep", type=float, default=1.0)
    parser.add_argument("--finra-user-agent", default="atx-impl FINRA dataset loader")
    parser.add_argument("--finra-feature-set", default="finra_short_interest_v1")
    parser.add_argument("--finra-feature-min-cross-section", type=int, default=20)

    parser.add_argument("--sec-13f-dataset-url")
    parser.add_argument("--sec-13f-cache-dir", type=Path, default=ThirteenFOptions().cache_dir)
    parser.add_argument("--cusips", default=AAPL_CUSIP)
    parser.add_argument("--full-13f-holdings", action="store_true")
    parser.add_argument("--sec-13f-chunk-size", type=int, default=200_000)
    parser.add_argument("--sec-13f-request-timeout", type=int, default=180)
    parser.add_argument("--sec-13f-user-agent", default="atx-impl 13F dataset loader nathan.tormaschy@gmail.com")
    parser.add_argument("--sec-13f-ownership-feature-set", default="sec_13f_ownership_v1")
    parser.add_argument("--compute-source-hash", action="store_true")

    parser.add_argument("--identifier-source-dataset-id", default="sec_13f")
    parser.add_argument("--identifier-source-period")
    parser.add_argument("--identifier-min-confidence", type=float, default=0.8)
    parser.add_argument("--identifier-include-already-mapped", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--identifier-min-accept-confidence", type=float, default=0.98)
    parser.add_argument("--identifier-min-review-confidence", type=float, default=0.8)
    parser.add_argument("--identifier-accept-statuses", default="already_mapped")
    parser.add_argument("--identifier-review-statuses", default="proposed,conflict")
    parser.add_argument("--do-not-apply-identifier-decisions", action="store_true")
    parser.add_argument("--identifier-decision-method", default="auto_confidence_status_policy_v1")
    parser.add_argument("--identifier-decided-by", default="system:auto_identifier_resolution_v1")

    parser.add_argument("--macro-series", default=",".join(DEFAULT_SERIES))
    parser.add_argument("--macro-start-date", type=parse_date)
    parser.add_argument("--macro-end-date", type=parse_date)
    parser.add_argument("--macro-request-timeout", type=int, default=60)
    parser.add_argument("--macro-user-agent", default="atx-impl macro loader nathan.tormaschy@gmail.com")

    parser.add_argument("--skip-lake-export", action="store_true")
    parser.add_argument("--lake-root", type=Path, default=DEFAULT_LAKE_ROOT)
    parser.add_argument("--lake-objects", help="Comma-separated table/view list. Defaults to curated lake objects.")
    parser.add_argument("--skip-lake-validation", action="store_true")
    parser.add_argument("--allow-lake-validation-failures", action="store_true")

    parser.add_argument("--skip-quality", action="store_true")
    parser.add_argument("--no-record-quality", action="store_true")
    parser.add_argument("--allow-quality-failures", action="store_true")
    parser.add_argument("--daily-macro-stale-days", type=int, default=10)
    parser.add_argument("--monthly-macro-stale-days", type=int, default=70)
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    if args.max_retries < 0:
        raise ValueError("--max-retries must be >= 0")
    if args.retry_delay_seconds < 0:
        raise ValueError("--retry-delay-seconds must be >= 0")
    for name in ("sec_request_timeout", "nasdaq_request_timeout", "finra_request_timeout", "sec_13f_request_timeout", "macro_request_timeout", "xbrl_context_request_timeout"):
        if getattr(args, name) < 1:
            raise ValueError(f"--{name.replace('_', '-')} must be positive")
    if args.xbrl_request_timeout < 1:
        raise ValueError("--xbrl-request-timeout must be positive")
    if args.sec_company_facts_symbol_limit is not None and args.sec_company_facts_symbol_limit < 1:
        raise ValueError("--sec-company-facts-symbol-limit must be positive")
    if args.xbrl_context_max_filings < 1:
        raise ValueError("--xbrl-context-max-filings must be positive")
    if args.xbrl_context_max_filings_per_symbol is not None and args.xbrl_context_max_filings_per_symbol < 1:
        raise ValueError("--xbrl-context-max-filings-per-symbol must be positive")
    if not args.skip_xbrl_taxonomy and not split_csv(args.xbrl_taxonomy_urls):
        raise ValueError("--xbrl-taxonomy-urls must include at least one URL unless --skip-xbrl-taxonomy is set")
    if args.finra_limit < 1 or args.finra_limit > 5000:
        raise ValueError("--finra-limit must be between 1 and 5000")
    if args.finra_limit_dates is not None and args.finra_limit_dates < 1:
        raise ValueError("--finra-limit-dates must be positive")
    if args.finra_feature_min_cross_section < 1:
        raise ValueError("--finra-feature-min-cross-section must be positive")
    if args.finra_request_retries < 1:
        raise ValueError("--finra-request-retries must be positive")
    if args.finra_retry_sleep < 0:
        raise ValueError("--finra-retry-sleep must be >= 0")
    if args.finra_start_date and args.finra_end_date and args.finra_start_date > args.finra_end_date:
        raise ValueError("--finra-start-date cannot be after --finra-end-date")
    if args.sec_13f_chunk_size < 1:
        raise ValueError("--sec-13f-chunk-size must be positive")
    if not args.full_13f_holdings and not normalize_cusips_arg(args.cusips):
        raise ValueError("--cusips must include at least one CUSIP unless --full-13f-holdings is set")
    for name in ("identifier_min_confidence", "identifier_min_accept_confidence", "identifier_min_review_confidence"):
        value = getattr(args, name)
        if value < 0 or value > 1:
            raise ValueError(f"--{name.replace('_', '-')} must be in [0, 1]")
    if args.identifier_min_review_confidence > args.identifier_min_accept_confidence:
        raise ValueError("--identifier-min-review-confidence cannot exceed --identifier-min-accept-confidence")
    if args.macro_start_date and args.macro_end_date and args.macro_start_date > args.macro_end_date:
        raise ValueError("--macro-start-date cannot be after --macro-end-date")
    if args.fundamental_feature_start_date and args.fundamental_feature_end_date and args.fundamental_feature_start_date > args.fundamental_feature_end_date:
        raise ValueError("--fundamental-feature-start-date cannot be after --fundamental-feature-end-date")
    if args.daily_macro_stale_days < 1 or args.monthly_macro_stale_days < 1:
        raise ValueError("Macro staleness thresholds must be positive")


def main() -> int:
    args = parse_args()
    validate_args(args)
    symbols = resolve_symbols(args)
    if not symbols:
        raise ValueError("No symbols resolved for public-source bootstrap")

    jobs = run_registered_jobs(args.db_path, args, symbols)
    lake_export = {"skipped": True}
    lake_validation = {"skipped": True, "problem_count": 0}
    watermarks = {"rows_upserted": 0, "selected": {}}
    quality = {"skipped": True, "passed": 0, "warnings": 0, "failed": 0, "failures": []}
    row_counts: dict[str, int] = {}
    summaries: dict[str, Any] = {}

    if not args.register_only:
        lake_export = export_lake(args.db_path, args)
        lake_validation = validate_lake(args.db_path, args, lake_export)
        watermarks = refresh_watermarks(args.db_path)
        quality = run_quality(args.db_path, args)
        with DuckDBStore(args.db_path, read_only=True) as store:
            row_counts = collect_row_counts(store)
            summaries = collect_public_summaries(store)

    output = {
        "db_path": str(args.db_path),
        "symbols": list(symbols),
        "symbol_count": len(symbols),
        "jobs": jobs,
        "lake_export": lake_export,
        "lake_validation": lake_validation,
        "watermarks": watermarks,
        "quality": quality,
        "row_counts": row_counts,
        "summaries": summaries,
    }
    print(json.dumps(output, indent=2, default=str))
    if lake_validation.get("problem_count", 0) and not args.allow_lake_validation_failures:
        return 1
    if quality.get("failed", 0) and not args.allow_quality_failures:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
