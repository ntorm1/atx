#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db.ticker_history import (
    DEFAULT_TICKER_HISTORY_ZIP,
    TickerHistoryArchiveProfileOptions,
    profile_ticker_history_archive,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Profile the local tbltickerhistory ZIP without loading it.")
    parser.add_argument("--zip-path", type=Path, default=DEFAULT_TICKER_HISTORY_ZIP)
    parser.add_argument("--chunk-size", type=int, default=200_000)
    parser.add_argument("--max-chunks", type=int, default=3)
    parser.add_argument("--top-n", type=int, default=25)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    profile = profile_ticker_history_archive(
        TickerHistoryArchiveProfileOptions(
            zip_path=args.zip_path,
            chunk_size=args.chunk_size,
            max_chunks=args.max_chunks,
            top_n=args.top_n,
        )
    )
    print(json.dumps(profile, indent=2, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
