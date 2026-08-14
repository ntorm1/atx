#!/usr/bin/env python
"""Stream the local broad-bar archive and publish breadth metadata without loading it."""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
from zipfile import ZipFile

import pandas as pd

from atx_db.ticker_history import DEFAULT_TICKER_HISTORY_ZIP


def _write(path: Path, payload: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(payload, indent=2, sort_keys=True, default=str) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zip-path", type=Path, default=DEFAULT_TICKER_HISTORY_ZIP)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("research/broad-bar-archive-breadth.json"),
    )
    parser.add_argument("--chunk-size", type=int, default=1_000_000)
    args = parser.parse_args()
    started = time.time()
    rows = 0
    chunks = 0
    all_security_ids: set[str] = set()
    all_symbols: set[str] = set()
    latest_date: str | None = None
    latest_security_ids: set[str] = set()
    latest_symbols: set[str] = set()
    with ZipFile(args.zip_path) as archive:
        members = archive.namelist()
        if len(members) != 1:
            raise ValueError("expected exactly one broad-bar archive member")
        with archive.open(members[0]) as source:
            reader = pd.read_csv(
                source,
                sep="\t",
                dtype=str,
                keep_default_na=False,
                usecols=["tradingDate", "securityID", "ticker_tk", "todayTicker"],
                chunksize=args.chunk_size,
            )
            for chunk in reader:
                chunks += 1
                rows += len(chunk)
                securities = chunk["securityID"].loc[lambda value: value.ne("")]
                symbols = chunk["todayTicker"].where(
                    chunk["todayTicker"].ne(""), chunk["ticker_tk"]
                )
                symbols = symbols.loc[lambda value: value.ne("")]
                all_security_ids.update(securities.unique().tolist())
                all_symbols.update(symbols.unique().tolist())
                chunk_latest = chunk["tradingDate"].max()
                if latest_date is None or chunk_latest > latest_date:
                    latest_date = chunk_latest
                    latest_security_ids.clear()
                    latest_symbols.clear()
                if chunk_latest == latest_date:
                    latest = chunk.loc[chunk["tradingDate"].eq(latest_date)]
                    latest_security_ids.update(
                        latest["securityID"].loc[lambda value: value.ne("")].unique().tolist()
                    )
                    latest_tickers = latest["todayTicker"].where(
                        latest["todayTicker"].ne(""), latest["ticker_tk"]
                    )
                    latest_symbols.update(
                        latest_tickers.loc[lambda value: value.ne("")].unique().tolist()
                    )
                if chunks % 10 == 0:
                    _write(
                        args.output,
                        {
                            "status": "running",
                            "chunks_scanned": chunks,
                            "rows_scanned": rows,
                            "latest_date_seen": latest_date,
                            "elapsed_seconds": round(time.time() - started, 1),
                        },
                    )
    _write(
        args.output,
        {
            "status": "complete",
            "archive": str(args.zip_path),
            "chunks_scanned": chunks,
            "rows_scanned": rows,
            "distinct_security_ids": len(all_security_ids),
            "distinct_symbols": len(all_symbols),
            "latest_date": latest_date,
            "latest_date_security_ids": len(latest_security_ids),
            "latest_date_symbols": len(latest_symbols),
            "elapsed_seconds": round(time.time() - started, 1),
        },
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
