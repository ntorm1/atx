#!/usr/bin/env python
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import uuid
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore, FinraShortInterestDataset, FinraShortInterestOptions
from atx_db.finra import discover_settlement_dates, parse_date, request_session, subtract_years
from atx_db.short_interest_features import ShortInterestFeatureDataset, ShortInterestFeatureOptions
from atx_db.watermarks import refresh_warehouse_watermarks
from atx_db.warehouse import json_dumps, now_utc_naive


def compact_dataset_result(result: Any) -> dict[str, Any]:
    details = dict(result.details)
    symbols = details.pop("symbols", [])
    details["symbol_count"] = len(symbols)
    details["sample_symbols"] = symbols[:10]
    return {
        "dataset_id": result.dataset_id,
        "rows_loaded": result.rows_loaded,
        "source": result.source,
        "run_id": result.run_id,
        "details": details,
    }


def existing_finra_symbol_counts(db_path: Path, dates: list[dt.date]) -> dict[dt.date, int]:
    if not dates:
        return {}
    with DuckDBStore(db_path, read_only=True) as store:
        store.con.execute("CREATE OR REPLACE TEMP TABLE finra_backfill_dates(settlement_date DATE)")
        store.con.executemany("INSERT INTO finra_backfill_dates VALUES (?)", [(value,) for value in dates])
        rows = store.con.execute(
            """
            SELECT d.settlement_date, count(DISTINCT f.symbol) AS symbol_count
            FROM finra_backfill_dates d
            LEFT JOIN finra_short_interest f
              ON f.settlement_date = d.settlement_date
            GROUP BY 1
            """
        ).fetchall()
    return {row[0]: int(row[1]) for row in rows}


def discover_dates(args: argparse.Namespace) -> list[dict[str, Any]]:
    session = request_session(args.user_agent)
    settlement_totals = discover_settlement_dates(
        session=session,
        api_url=args.api_url,
        start_date=args.start_date,
        end_date=args.end_date,
        timeout=args.request_timeout,
        max_retries=args.max_retries,
        retry_sleep=args.retry_sleep,
    )
    items = list(settlement_totals.items())
    if args.date_order == "desc":
        items = list(reversed(items))
    dates = [settlement_date for settlement_date, _ in items]
    existing_counts = existing_finra_symbol_counts(args.db_path, dates)

    candidates: list[dict[str, Any]] = []
    selected_count = 0
    for settlement_date, source_total in items:
        existing_symbols = existing_counts.get(settlement_date, 0)
        already_broad = (
            not args.force
            and args.skip_existing_min_symbols is not None
            and existing_symbols >= args.skip_existing_min_symbols
        )
        status = "skipped_existing_broad" if already_broad else "selected"
        if status == "selected":
            if selected_count >= args.limit_dates:
                status = "not_selected_limit_reached"
            else:
                selected_count += 1
        candidates.append(
            {
                "settlement_date": settlement_date,
                "source_total": int(source_total),
                "existing_symbols": existing_symbols,
                "status": status,
            }
        )
    return candidates


def load_dates(args: argparse.Namespace, dates: list[dt.date]) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    dataset = FinraShortInterestDataset()
    with DuckDBStore(args.db_path) as store:
        for settlement_date in dates:
            result = dataset.run(
                store,
                FinraShortInterestOptions(
                    api_url=args.api_url,
                    start_date=settlement_date,
                    end_date=settlement_date,
                    limit=args.limit,
                    request_timeout=args.request_timeout,
                    max_retries=args.max_retries,
                    retry_sleep=args.retry_sleep,
                    limit_dates=1,
                    date_order=args.date_order,
                    user_agent=args.user_agent,
                ),
            )
            results.append(compact_dataset_result(result))
    return results


def rebuild_features(args: argparse.Namespace) -> dict[str, Any] | None:
    if not args.build_features:
        return None
    with DuckDBStore(args.db_path) as store:
        result = ShortInterestFeatureDataset().run(
            store,
            ShortInterestFeatureOptions(
                feature_set=args.feature_set,
                min_cross_section=args.min_cross_section,
            ),
        )
    return compact_dataset_result(result)


def refresh_watermarks(args: argparse.Namespace) -> dict[str, Any] | None:
    if not args.refresh_watermarks:
        return None
    with DuckDBStore(args.db_path) as store:
        result = refresh_warehouse_watermarks(store)
    selected = [
        row
        for row in result.watermarks
        if row["dataset_id"]
        in {"finra_short_interest", "finra_short_interest_backfills", "finra_short_interest_features", "feature_lineage"}
    ]
    return {"rows_upserted": result.rows_upserted, "selected": selected}


def record_manifest(
    args: argparse.Namespace,
    *,
    started_at: dt.datetime,
    candidates: list[dict[str, Any]],
    selected_dates: list[dt.date],
    load_results: list[dict[str, Any]],
    feature_result: dict[str, Any] | None,
    watermark_result: dict[str, Any] | None,
) -> dict[str, Any]:
    finished_at = now_utc_naive()
    manifest_id = str(uuid.uuid4())
    status = "succeeded" if load_results else "skipped"
    source_row_count = sum(int(result["rows_loaded"]) for result in load_results)
    feature_row_count = None if feature_result is None else int(feature_result["rows_loaded"])
    params = {
        key: str(value) if isinstance(value, Path) else value
        for key, value in vars(args).items()
    }
    row = {
        "manifest_id": manifest_id,
        "status": status,
        "start_date": args.start_date,
        "end_date": args.end_date,
        "date_order": args.date_order,
        "limit_dates": args.limit_dates,
        "skip_existing_min_symbols": args.skip_existing_min_symbols,
        "force": args.force,
        "candidate_count": len(candidates),
        "selected_date_count": len(selected_dates),
        "loaded_date_count": len(load_results),
        "source_row_count": source_row_count,
        "feature_row_count": feature_row_count,
        "selected_dates_json": json_dumps(selected_dates),
        "candidates_json": json_dumps(candidates),
        "load_results_json": json_dumps(load_results),
        "feature_result_json": None if feature_result is None else json_dumps(feature_result),
        "watermarks_json": None if watermark_result is None else json_dumps(watermark_result),
        "params_json": json_dumps(params),
        "source": args.api_url,
        "started_at": started_at,
        "finished_at": finished_at,
    }
    with DuckDBStore(args.db_path) as store:
        store.con.execute(
            """
            INSERT INTO finra_short_interest_backfill_manifests (
                manifest_id,
                status,
                start_date,
                end_date,
                date_order,
                limit_dates,
                skip_existing_min_symbols,
                force,
                candidate_count,
                selected_date_count,
                loaded_date_count,
                source_row_count,
                feature_row_count,
                selected_dates_json,
                candidates_json,
                load_results_json,
                feature_result_json,
                watermarks_json,
                params_json,
                source,
                started_at,
                finished_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            [
                row["manifest_id"],
                row["status"],
                row["start_date"],
                row["end_date"],
                row["date_order"],
                row["limit_dates"],
                row["skip_existing_min_symbols"],
                row["force"],
                row["candidate_count"],
                row["selected_date_count"],
                row["loaded_date_count"],
                row["source_row_count"],
                row["feature_row_count"],
                row["selected_dates_json"],
                row["candidates_json"],
                row["load_results_json"],
                row["feature_result_json"],
                row["watermarks_json"],
                row["params_json"],
                row["source"],
                row["started_at"],
                row["finished_at"],
            ],
        )
    return row


def update_manifest_watermarks(
    db_path: Path,
    manifest_id: str,
    watermark_result: dict[str, Any] | None,
) -> None:
    if watermark_result is None:
        return
    with DuckDBStore(db_path) as store:
        store.con.execute(
            """
            UPDATE finra_short_interest_backfill_manifests
            SET watermarks_json = ?
            WHERE manifest_id = ?
            """,
            [json_dumps(watermark_result), manifest_id],
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compact all-symbol FINRA short-interest settlement-date backfill with feature rebuild."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--start-date", type=parse_date, default=subtract_years(dt.date.today(), 1))
    parser.add_argument("--end-date", type=parse_date, default=dt.date.today())
    parser.add_argument("--api-url", default=FinraShortInterestOptions.api_url)
    parser.add_argument("--limit", type=int, default=5000)
    parser.add_argument("--limit-dates", type=int, default=1)
    parser.add_argument("--date-order", choices=("asc", "desc"), default="desc")
    parser.add_argument("--skip-existing-min-symbols", type=int, default=1000)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--request-timeout", type=int, default=120)
    parser.add_argument("--max-retries", type=int, default=5)
    parser.add_argument("--retry-sleep", type=float, default=1.0)
    parser.add_argument("--user-agent", default=FinraShortInterestOptions.user_agent)
    parser.add_argument("--build-features", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--feature-set", default=ShortInterestFeatureOptions().feature_set)
    parser.add_argument("--min-cross-section", type=int, default=ShortInterestFeatureOptions().min_cross_section)
    parser.add_argument("--refresh-watermarks", action=argparse.BooleanOptionalAction, default=True)
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    if args.start_date > args.end_date:
        raise ValueError("--start-date cannot be after --end-date")
    if args.limit < 1 or args.limit > 5000:
        raise ValueError("--limit must be between 1 and 5000")
    if args.limit_dates < 1:
        raise ValueError("--limit-dates must be positive")
    if args.skip_existing_min_symbols is not None and args.skip_existing_min_symbols < 1:
        raise ValueError("--skip-existing-min-symbols must be positive")
    if args.request_timeout < 1:
        raise ValueError("--request-timeout must be positive")
    if args.max_retries < 1:
        raise ValueError("--max-retries must be positive")
    if args.retry_sleep < 0:
        raise ValueError("--retry-sleep must be >= 0")
    if args.min_cross_section < 1:
        raise ValueError("--min-cross-section must be positive")


def main() -> int:
    args = parse_args()
    validate_args(args)
    started_at = now_utc_naive()
    candidates = discover_dates(args)
    selected_dates = [row["settlement_date"] for row in candidates if row["status"] == "selected"]
    load_results = load_dates(args, selected_dates)
    feature_result = rebuild_features(args) if load_results else None
    manifest = record_manifest(
        args,
        started_at=started_at,
        candidates=candidates,
        selected_dates=selected_dates,
        load_results=load_results,
        feature_result=feature_result,
        watermark_result=None,
    )
    watermark_result = refresh_watermarks(args)
    update_manifest_watermarks(args.db_path, manifest["manifest_id"], watermark_result)
    print(
        json.dumps(
            {
                "db_path": str(args.db_path),
                "manifest_id": manifest["manifest_id"],
                "status": manifest["status"],
                "window": {"start_date": args.start_date, "end_date": args.end_date},
                "date_order": args.date_order,
                "limit_dates": args.limit_dates,
                "selected_dates": selected_dates,
                "candidates": candidates,
                "load_results": load_results,
                "feature_result": feature_result,
                "watermarks": watermark_result,
            },
            indent=2,
            default=str,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
