#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore, JobManager
from db.jobs import DATASET_REGISTRY
from db.ticker_history import DEFAULT_TICKER_HISTORY_ZIP
from db.warehouse import symbol_key


def parse_json(value: str | None = None, params_file: Path | None = None) -> dict:
    if value and params_file:
        raise ValueError("Use either --params-json or --params-file, not both")
    if params_file:
        return json.loads(params_file.read_text(encoding="utf-8-sig"))
    if not value:
        return {}
    return json.loads(value)


def parse_csv(value: str | None) -> list[str]:
    if not value:
        return []
    return [part.strip() for part in value.split(",") if part.strip()]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Manage atx-impl DuckDB warehouse ETL jobs.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list-datasets")
    sub.add_parser("list-jobs")
    run_all = sub.add_parser("run-all")
    run_all.add_argument("--full-rebuild", action="store_true")
    run_all.add_argument("--resume", metavar="RUN_ID")

    seed = sub.add_parser("seed-defaults")
    seed.add_argument("--symbols", default="AAPL")
    seed.add_argument("--ticker-history-zip", type=Path, default=DEFAULT_TICKER_HISTORY_ZIP)
    seed.add_argument("--ticker-max-chunks", type=int)
    seed.add_argument("--max-retries", type=int, default=1)
    seed.add_argument("--retry-delay-seconds", type=float, default=10.0)

    register = sub.add_parser("register")
    register.add_argument("job_name")
    register.add_argument("dataset_id", choices=sorted(DATASET_REGISTRY))
    register.add_argument("--params-json")
    register.add_argument("--params-file", type=Path)
    register.add_argument("--schedule")
    register.add_argument("--dependencies", help="Comma-separated upstream job names.")
    register.add_argument("--disabled", action="store_true")
    register.add_argument("--max-retries", type=int, default=0)
    register.add_argument("--retry-delay-seconds", type=float, default=0.0)

    run = sub.add_parser("run")
    run.add_argument("job_name")
    run.add_argument("--params-json")
    run.add_argument("--params-file", type=Path)
    run.add_argument("--max-retries", type=int)
    run.add_argument("--retry-delay-seconds", type=float)

    run_dataset = sub.add_parser("run-dataset")
    run_dataset.add_argument("dataset_id", choices=sorted(DATASET_REGISTRY))
    run_dataset.add_argument("--params-json")
    run_dataset.add_argument("--params-file", type=Path)
    run_dataset.add_argument("--max-retries", type=int, default=0)
    run_dataset.add_argument("--retry-delay-seconds", type=float, default=0.0)

    return parser.parse_args()


def dataset_result_json(result) -> dict:
    return {
        "dataset_id": result.dataset_id,
        "rows_loaded": result.rows_loaded,
        "source": result.source,
        "run_id": result.run_id,
        "details": result.details,
    }


def run_all_json(run_result, results) -> dict:
    return {
        "results": [dataset_result_json(result) for result in results],
        "run_id": run_result.run_id,
        "status": run_result.status,
        "dataset_order": list(run_result.dataset_order),
    }


def main() -> int:
    args = parse_args()
    with DuckDBStore(args.db_path) as store:
        manager = JobManager(store)
        if args.command == "list-datasets":
            print(json.dumps(sorted(DATASET_REGISTRY), indent=2))
            return 0
        if args.command == "list-jobs":
            rows = store.con.execute(
                """
                SELECT
                    job_name,
                    dataset_id,
                    enabled,
                    max_retries,
                    retry_delay_seconds,
                    schedule,
                    dependencies_json
                FROM etl_job_definitions
                ORDER BY job_name
                """
            ).fetchall()
            print(
                json.dumps(
                    {
                        "enabled_order": manager.enabled_job_order(),
                        "jobs": [
                            {
                                "job_name": row[0],
                                "dataset_id": row[1],
                                "enabled": row[2],
                                "max_retries": row[3],
                                "retry_delay_seconds": row[4],
                                "schedule": row[5],
                                "dependencies_json": row[6],
                            }
                            for row in rows
                        ],
                    },
                    indent=2,
                    default=str,
                )
            )
            return 0
        if args.command == "seed-defaults":
            symbols = tuple(symbol_key(part) for part in args.symbols.split(",") if symbol_key(part))
            manager.seed_default_jobs(
                symbols=symbols,
                ticker_history_zip=args.ticker_history_zip,
                max_chunks=args.ticker_max_chunks,
                max_retries=args.max_retries,
                retry_delay_seconds=args.retry_delay_seconds,
            )
            print(
                json.dumps(
                    {
                        "seeded": True,
                        "symbols": symbols,
                        "max_retries": args.max_retries,
                        "retry_delay_seconds": args.retry_delay_seconds,
                    },
                    indent=2,
                )
            )
            return 0
        if args.command == "register":
            manager.register_job(
                job_name=args.job_name,
                dataset_id=args.dataset_id,
                params=parse_json(args.params_json, args.params_file),
                enabled=not args.disabled,
                schedule=args.schedule,
                dependencies=parse_csv(args.dependencies),
                max_retries=args.max_retries,
                retry_delay_seconds=args.retry_delay_seconds,
            )
            print(json.dumps({"registered": args.job_name, "dataset_id": args.dataset_id}, indent=2))
            return 0
        if args.command == "run":
            result = manager.run_job(
                args.job_name,
                override_params=parse_json(args.params_json, args.params_file),
                max_retries=args.max_retries,
                retry_delay_seconds=args.retry_delay_seconds,
            )
        elif args.command == "run-all":
            run_result = manager.run_all_enabled(
                full_rebuild=args.full_rebuild,
                resume=args.resume,
            )
            results = manager.run_all_results(
                run_result.run_id,
                run_result.dataset_order,
            )
            print(
                json.dumps(
                    run_all_json(run_result, results),
                    indent=2,
                    default=str,
                )
            )
            return 0
        else:
            result = manager.run_dataset(
                args.dataset_id,
                parse_json(args.params_json, args.params_file),
                max_retries=args.max_retries,
                retry_delay_seconds=args.retry_delay_seconds,
            )
    print(
        json.dumps(
            dataset_result_json(result),
            indent=2,
            default=str,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
