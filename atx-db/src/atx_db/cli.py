"""Operational command line for the atx-db warehouse."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import logging
import uuid
from collections.abc import Sequence
from dataclasses import asdict
from pathlib import Path
from typing import Any

from .connection import DEFAULT_DB_PATH, DuckDBStore
from .migrations import MIGRATIONS
from .openfigi_signals import OpenFigiSignalMapOptions, map_signal_cusips
from .thirteenf_amendments import refresh_thirteenf_amendments
from .thirteenf_analysis import write_thirteenf_analysis_report
from .thirteenf_archive import (
    ThirteenFArchiveBackfillOptions,
    archive_backfill_result,
    backfill_archives,
)
from .thirteenf_backtest import (
    DEFAULT_HORIZONS,
    DEFAULT_MIDCAP_MAX_USD,
    DEFAULT_MIDCAP_MIN_USD,
    ThirteenFSignalPriceOptions,
    load_signal_price_history,
    refresh_thirteenf_signal_backtest,
)
from .thirteenf_signals import refresh_thirteenf_consensus_signals


def _json(value: Any) -> None:
    print(json.dumps(value, default=str, sort_keys=True))


def _table_exists(store: DuckDBStore, table_name: str) -> bool:
    row = store.con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'main' AND table_name = ?",
        [table_name],
    ).fetchone()
    if row is None:
        raise RuntimeError("Could not inspect warehouse tables")
    return bool(row[0])


def _configure_analytical_session(
    store: DuckDBStore, *, memory_limit: str, threads: int
) -> None:
    if threads < 1:
        raise ValueError("threads must be positive")
    store.con.execute("SET memory_limit = ?", [memory_limit])
    store.con.execute("SET threads = ?", [threads])
    store.con.execute("SET preserve_insertion_order = false")


def warehouse_status(db_path: Path) -> dict[str, Any]:
    if not db_path.is_file():
        return {
            "status": "missing",
            "db_path": str(db_path),
            "target_schema_version": max(migration.version for migration in MIGRATIONS),
        }
    with DuckDBStore(db_path, read_only=True) as store:
        version = None
        if _table_exists(store, "schema_migrations"):
            version_row = store.con.execute(
                "SELECT max(try_cast(version AS INTEGER)) FROM schema_migrations"
            ).fetchone()
            if version_row is None:
                raise RuntimeError("Could not inspect schema migration version")
            version = version_row[0]
        tables: dict[str, dict[str, Any]] = {}
        table_specs = {
            "sec_company_facts": "period_end",
            "equity_daily_bars": "trade_date",
            "thirteenf_submissions": "period_of_report",
            "thirteenf_holdings": None,
            "thirteenf_effective_positions": "report_period",
            "thirteenf_amendment_rates": "report_period",
        }
        for table_name, date_column in table_specs.items():
            if not _table_exists(store, table_name):
                continue
            if date_column:
                coverage_row = store.con.execute(
                    f"SELECT count(*), min({date_column}), max({date_column}) FROM {table_name}"
                ).fetchone()
                if coverage_row is None:
                    raise RuntimeError(f"Could not inspect {table_name} coverage")
                count, minimum, maximum = coverage_row
                tables[table_name] = {"rows": int(count), "min_date": minimum, "max_date": maximum}
            else:
                count_row = store.con.execute(f"SELECT count(*) FROM {table_name}").fetchone()
                if count_row is None:
                    raise RuntimeError(f"Could not inspect {table_name} row count")
                count = count_row[0]
                tables[table_name] = {"rows": int(count)}
    target = max(migration.version for migration in MIGRATIONS)
    return {
        "status": "ready" if version == target else "migration_required",
        "db_path": str(db_path),
        "db_bytes": db_path.stat().st_size,
        "schema_version": version,
        "target_schema_version": target,
        "tables": tables,
    }


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="atx-db", description="Operate the atx-db warehouse")
    commands = parser.add_subparsers(dest="command", required=True)

    status = commands.add_parser("status", help="Report schema and core dataset coverage")
    status.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    status.add_argument("--strict", action="store_true", help="Exit nonzero unless the schema is current")

    backfill = commands.add_parser("backfill-13f", help="Load SEC quarterly Form 13F archives")
    backfill.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    backfill.add_argument("--start", type=dt.date.fromisoformat, default=dt.date(2013, 4, 1))
    backfill.add_argument("--end", type=dt.date.fromisoformat)
    backfill.add_argument("--url", action="append", dest="urls")
    backfill.add_argument("--cache-dir", type=Path)
    backfill.add_argument("--extract-dir", type=Path)
    backfill.add_argument("--keep-extracted", action="store_true")
    backfill.add_argument("--no-hash", action="store_true")
    backfill.add_argument("--continue-on-error", action="store_true")
    backfill.add_argument("--user-agent")
    backfill.add_argument("--run-id")
    backfill.add_argument("--replace-loaded", action="store_true")
    backfill.add_argument("--keep-indexes-during-load", action="store_true")

    refresh = commands.add_parser(
        "refresh-13f-amendments", help="Reconstruct effective portfolios and amendment rates"
    )
    refresh.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    refresh.add_argument("--start", type=dt.date.fromisoformat)
    refresh.add_argument("--end", type=dt.date.fromisoformat)
    refresh.add_argument("--minimum-history-quarters", type=int, default=24)
    refresh.add_argument("--spike-zscore", type=float, default=2.0)
    refresh.add_argument("--zero-variance-zscore", type=float, default=10.0)
    refresh.add_argument("--memory-limit", default="4GB")
    refresh.add_argument("--threads", type=int, default=4)
    refresh.add_argument(
        "--skip-effective-positions",
        action="store_true",
        help="Build corrections and rates without materializing every unchanged position",
    )
    refresh.add_argument("--run-id")

    signals = commands.add_parser(
        "refresh-13f-signals", help="Build multi-filer consensus signals and disclosed-exit outcomes"
    )
    signals.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    signals.add_argument("--start", type=dt.date.fromisoformat)
    signals.add_argument("--end", type=dt.date.fromisoformat)
    signals.add_argument("--minimum-distinct-filers", type=int, default=3)
    signals.add_argument("--stress-zscore", type=float, default=1.5)
    signals.add_argument("--memory-limit", default="4GB")
    signals.add_argument("--threads", type=int, default=4)
    signals.add_argument("--run-id")

    mapping = commands.add_parser(
        "map-13f-signal-instruments", help="Map consensus-signal CUSIPs through OpenFIGI v3"
    )
    mapping.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    mapping.add_argument("--start", type=dt.date.fromisoformat)
    mapping.add_argument("--end", type=dt.date.fromisoformat)
    mapping.add_argument("--cache-dir", type=Path)
    mapping.add_argument("--max-rank-per-quarter", type=int, default=20)
    mapping.add_argument("--include-stress", action="store_true")
    mapping.add_argument("--replace", action="store_true")
    mapping.add_argument("--run-id")

    prices = commands.add_parser(
        "load-13f-signal-prices", help="Load local daily history only for mapped signal tickers"
    )
    prices.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    prices.add_argument("--archive-path", type=Path, required=True)
    prices.add_argument("--start", type=dt.date.fromisoformat)
    prices.add_argument("--end", type=dt.date.fromisoformat)
    prices.add_argument("--chunk-size", type=int, default=200_000)
    prices.add_argument("--run-id")

    backtest = commands.add_parser(
        "backtest-13f-signals", help="Evaluate mapped signals on availability-safe daily bars"
    )
    backtest.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    backtest.add_argument("--horizon", type=int, action="append", dest="horizons")
    backtest.add_argument("--price-source", default="tbltickerhistory3_10y")
    backtest.add_argument("--one-way-slippage-bps", type=float, default=5.0)
    backtest.add_argument("--max-rank-per-quarter", type=int, default=20)
    backtest.add_argument("--include-stress", action="store_true")
    backtest.add_argument("--minimum-market-cap-usd", type=float, default=DEFAULT_MIDCAP_MIN_USD)
    backtest.add_argument("--maximum-market-cap-usd", type=float, default=DEFAULT_MIDCAP_MAX_USD)
    backtest.add_argument("--run-id")

    report = commands.add_parser("report-13f-analysis", help="Render the independent amendment analysis")
    report.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    report.add_argument(
        "--output",
        type=Path,
        default=Path("research") / "recreated-l1vsun-13f-amendment-analysis.md",
    )
    report.add_argument("--json-output", type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    args = _build_parser().parse_args(argv)
    if args.command == "status":
        status = warehouse_status(args.db_path)
        _json(status)
        return 0 if not args.strict or status["status"] == "ready" else 2

    if args.command == "backfill-13f":
        defaults = ThirteenFArchiveBackfillOptions()
        options = ThirteenFArchiveBackfillOptions(
            start=args.start,
            end=args.end,
            urls=None if args.urls is None else tuple(args.urls),
            cache_dir=args.cache_dir or defaults.cache_dir,
            extract_dir=args.extract_dir or defaults.extract_dir,
            keep_extracted=args.keep_extracted,
            compute_source_hash=not args.no_hash,
            user_agent=args.user_agent or defaults.user_agent,
            run_id=args.run_id or f"sec-13f-archive-{uuid.uuid4()}",
            fail_fast=not args.continue_on_error,
            replace_loaded_archives=args.replace_loaded,
            defer_indexes=not args.keep_indexes_during_load,
        )
        with DuckDBStore(args.db_path) as store:
            store.con.execute("PRAGMA disable_progress_bar")
            backfill_result = archive_backfill_result(backfill_archives(store, options))
        _json(asdict(backfill_result))
        return 0

    if args.command == "refresh-13f-amendments":
        with DuckDBStore(args.db_path) as store:
            store.con.execute("PRAGMA disable_progress_bar")
            _configure_analytical_session(
                store, memory_limit=args.memory_limit, threads=args.threads
            )
            amendment_result = refresh_thirteenf_amendments(
                store,
                start=args.start,
                end=args.end,
                minimum_history_quarters=args.minimum_history_quarters,
                spike_zscore=args.spike_zscore,
                zero_variance_zscore=args.zero_variance_zscore,
                materialize_effective_positions=not args.skip_effective_positions,
                run_id=args.run_id or f"13f-amendment-refresh-{uuid.uuid4()}",
            )
        _json(asdict(amendment_result))
        return 0

    if args.command == "refresh-13f-signals":
        with DuckDBStore(args.db_path) as store:
            store.con.execute("PRAGMA disable_progress_bar")
            _configure_analytical_session(
                store, memory_limit=args.memory_limit, threads=args.threads
            )
            signal_result = refresh_thirteenf_consensus_signals(
                store,
                start=args.start,
                end=args.end,
                minimum_distinct_filers=args.minimum_distinct_filers,
                stress_zscore=args.stress_zscore,
                run_id=args.run_id or f"13f-signal-refresh-{uuid.uuid4()}",
            )
        _json(asdict(signal_result))
        return 0

    if args.command == "map-13f-signal-instruments":
        mapping_defaults = OpenFigiSignalMapOptions()
        mapping_options = OpenFigiSignalMapOptions(
            start=args.start,
            end=args.end,
            cache_dir=args.cache_dir or mapping_defaults.cache_dir,
            maximum_rank_per_quarter=args.max_rank_per_quarter,
            include_stress_quarters=args.include_stress,
            replace=args.replace,
            run_id=args.run_id,
        )
        with DuckDBStore(args.db_path) as store:
            mapping_result = map_signal_cusips(store, mapping_options)
        _json(asdict(mapping_result))
        return 0

    if args.command == "load-13f-signal-prices":
        with DuckDBStore(args.db_path) as store:
            price_result = load_signal_price_history(
                store,
                ThirteenFSignalPriceOptions(
                    archive_path=args.archive_path,
                    start_date=args.start,
                    end_date=args.end,
                    chunk_size=args.chunk_size,
                    run_id=args.run_id,
                ),
            )
        _json(asdict(price_result))
        return 0

    if args.command == "backtest-13f-signals":
        with DuckDBStore(args.db_path) as store:
            store.con.execute("PRAGMA disable_progress_bar")
            backtest_result = refresh_thirteenf_signal_backtest(
                store,
                horizons=tuple(args.horizons or DEFAULT_HORIZONS),
                price_source=args.price_source,
                one_way_slippage_bps=args.one_way_slippage_bps,
                maximum_rank_per_quarter=args.max_rank_per_quarter,
                include_stress_quarters=args.include_stress,
                minimum_market_cap_usd=args.minimum_market_cap_usd,
                maximum_market_cap_usd=args.maximum_market_cap_usd,
                run_id=args.run_id,
            )
        _json(asdict(backtest_result))
        return 0

    if args.command == "report-13f-analysis":
        with DuckDBStore(args.db_path, read_only=True) as store:
            report_summary = write_thirteenf_analysis_report(
                store,
                args.output,
                json_path=args.json_output,
            )
        _json(report_summary)
        return 0

    raise AssertionError(f"Unhandled command: {args.command}")


if __name__ == "__main__":
    raise SystemExit(main())
