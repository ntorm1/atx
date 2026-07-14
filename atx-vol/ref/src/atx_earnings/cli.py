from __future__ import annotations

import argparse
import csv
import json
import sys
from datetime import date, datetime
from pathlib import Path

from .db import EarningsDatabase
from .pipeline import EarningsService
from .sources import NasdaqEarningsSource, WikipediaSP500Source, load_csv_snapshots


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="atx-earnings")
    parser.add_argument("--db", default="earnings.sqlite", help="SQLite database path")
    sub = parser.add_subparsers(dest="command", required=True)

    backfill = sub.add_parser("backfill", help="backfill two years and build daily wide rows")
    backfill.add_argument("--today", type=date.fromisoformat)
    backfill.add_argument("--history-days", type=int, default=730)
    backfill.add_argument("--horizon-days", type=int, default=365)
    backfill.add_argument("--request-delay", type=float, default=0.25)

    daily = sub.add_parser("daily", help="run the idempotent daily refresh/revision job")
    daily.add_argument("--today", type=date.fromisoformat)
    daily.add_argument("--revision-lookback-days", type=int, default=30)
    daily.add_argument("--universe-lookback-days", type=int, default=730)
    daily.add_argument("--horizon-days", type=int, default=365)
    daily.add_argument("--request-delay", type=float, default=0.25)

    imported = sub.add_parser("import-csv", help="append confirmed/alternate source snapshots")
    imported.add_argument("path", type=Path)
    imported.add_argument("--source", default="csv-confirmed")
    imported.add_argument("--observed-at", type=_datetime)

    build = sub.add_parser("build-reference", help="materialize a PIT wide reference range")
    build.add_argument("start", type=date.fromisoformat)
    build.add_argument("end", type=date.fromisoformat)
    build.add_argument("--known-at", type=_datetime)
    build.add_argument("--horizon-days", type=int, default=365)

    export = sub.add_parser("export", help="export the latest wide (date,symbol) rows")
    export.add_argument("path", type=Path)
    export.add_argument("--date", type=date.fromisoformat)
    export.add_argument("--symbol")

    status = sub.add_parser("status", help="show recent job runs and database counts")
    status.add_argument("--limit", type=int, default=10)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    with EarningsDatabase(args.db) as database:
        if args.command == "import-csv":
            snapshot_ids = load_csv_snapshots(
                database, args.path, source=args.source, observed_at=args.observed_at
            )
            print(json.dumps({"snapshots": len(snapshot_ids), "snapshot_ids": snapshot_ids}))
            return 0
        if args.command == "export":
            rows = list(
                database.iter_latest_reference(as_of_date=args.date, symbol=args.symbol)
            )
            _write_csv(args.path, rows)
            print(json.dumps({"rows": len(rows), "path": str(args.path)}))
            return 0
        if args.command == "status":
            _print_status(database, args.limit)
            return 0

        with NasdaqEarningsSource() as earnings, WikipediaSP500Source() as universe:
            service = EarningsService(
                database,
                earnings_source=earnings,
                universe_source=universe,
                request_delay_seconds=getattr(args, "request_delay", 0.0),
            )
            if args.command == "backfill":
                result = service.backfill(
                    today=args.today,
                    history_days=args.history_days,
                    horizon_days=args.horizon_days,
                )
            elif args.command == "daily":
                result = service.daily(
                    today=args.today,
                    revision_lookback_days=args.revision_lookback_days,
                    universe_lookback_days=args.universe_lookback_days,
                    horizon_days=args.horizon_days,
                )
            else:
                build_id = service.materialize_reference(
                    args.start,
                    args.end,
                    known_at=args.known_at,
                    horizon_days=args.horizon_days,
                )
                result = {"build_id": build_id}
        print(json.dumps(result, sort_keys=True))
        return 2 if result.get("status") == "PARTIAL" else 0


def _datetime(value: str) -> datetime:
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def _write_csv(path: Path, rows) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        if not rows:
            return
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys(), extrasaction="ignore")
        writer.writeheader()
        writer.writerows(dict(row) for row in rows)


def _print_status(database: EarningsDatabase, limit: int) -> None:
    counts = {
        table: database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        for table in (
            "source_snapshots",
            "earnings_observations",
            "universe_memberships",
            "reference_builds",
            "reference_rows",
        )
    }
    jobs = [
        dict(row)
        for row in database.connection.execute(
            "SELECT * FROM job_runs ORDER BY job_run_id DESC LIMIT ?", (limit,)
        )
    ]
    print(json.dumps({"counts": counts, "jobs": jobs}, indent=2, sort_keys=True))


if __name__ == "__main__":
    sys.exit(main())

