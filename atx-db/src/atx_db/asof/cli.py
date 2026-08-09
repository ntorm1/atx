from __future__ import annotations

from ._common import (
    DEFAULT_DB_PATH,
    Path,
    dt,
)


from .catalog import warehouse_catalog_asof


def _parse_csv_arg(value: str | None) -> list[str] | None:
    """Split a comma-separated CLI argument into a normalized list, or None if empty."""
    if not value:
        return None
    parsed = [part.strip() for part in value.split(",") if part.strip()]
    return parsed or None

def _build_arg_parser():
    import argparse

    parser = argparse.ArgumentParser(
        prog="python -m atx_db.asof",
        description="Point-in-time query CLI over atx_db readers.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    catalog_parser = subparsers.add_parser(
        "warehouse-catalog",
        help="Print the as-of warehouse data catalog (table_catalog + field_catalog + best-effort formula lineage).",
    )
    catalog_parser.add_argument(
        "--as-of", dest="as_of_date", type=dt.date.fromisoformat, required=True, help="As-of date (YYYY-MM-DD)."
    )
    catalog_parser.add_argument(
        "--as-of-ts", type=dt.datetime.fromisoformat, default=None, help="Optional as-of knowledge-time timestamp."
    )
    catalog_parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    catalog_parser.add_argument("--tables", help="Comma-separated table_name filter.")
    catalog_parser.add_argument("--layers", help="Comma-separated layer filter.")

    return parser

def main(argv: list[str] | None = None) -> int:
    parser = _build_arg_parser()
    args = parser.parse_args(argv)
    if args.command == "warehouse-catalog":
        frame = warehouse_catalog_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            tables=_parse_csv_arg(args.tables),
            layers=_parse_csv_arg(args.layers),
        )
        print(frame.to_string(index=False))
        return 0
    parser.error(f"unknown command: {args.command}")
    return 2
