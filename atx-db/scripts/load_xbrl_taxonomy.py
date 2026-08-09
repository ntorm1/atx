#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.xbrl_taxonomy import DEFAULT_XBRL_TAXONOMY_PACKAGE_URLS, XbrlTaxonomyOptions, refresh_xbrl_taxonomy


def split_csv(value: str | None) -> tuple[str, ...]:
    if not value:
        return ()
    return tuple(part.strip() for part in value.split(",") if part.strip())


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Load public FASB/XBRL taxonomy linkbase relationships.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--package-urls", default=",".join(DEFAULT_XBRL_TAXONOMY_PACKAGE_URLS))
    parser.add_argument("--request-timeout", type=int, default=120)
    parser.add_argument("--user-agent", default="atx-db XBRL taxonomy loader nathan.tormaschy@gmail.com")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    package_urls = split_csv(args.package_urls)
    if not package_urls:
        raise ValueError("--package-urls must include at least one URL")
    if args.request_timeout < 1:
        raise ValueError("--request-timeout must be positive")
    with DuckDBStore(args.db_path) as store:
        store.initialize()
        result = refresh_xbrl_taxonomy(
            store,
            XbrlTaxonomyOptions(
                package_urls=package_urls,
                request_timeout=args.request_timeout,
                user_agent=args.user_agent,
            ),
        )
    print(json.dumps({"db_path": str(args.db_path), **result}, indent=2, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
