#!/usr/bin/env python
"""Fetch FINRA short interest and Reg SHO daily short-volume history to files (X.2 fetch half, ruling C-91).

No DuckDB connection is opened: every file goes, gzip-compressed, under ``--out-dir`` (default
``<data dir>/raw/finra``) with one receipt line per request outcome in each root's ``receipts.jsonl``:

* ``short-interest/api``      FINRA Query API ``consolidatedShortInterest``, every settlement date it holds
  (exchange-listed + OTC, from settlement 2017-12-29), all rows per date in sorted pages;
* ``short-interest/cdn``      the official twice-monthly ``shrt<yyyymmdd>.csv`` files (from 2018-11);
* ``short-interest/archive``  the OTC-only static archive (settlements 2014-11-14 .. 2019-06-28);
* ``regsho-daily/<PREFIX>``   the daily short-volume files per facility (CNMS, FNSQ, FNQC, FNYX, FNRA, FORF).

Clocks recorded per file: short interest ``publication_date = settlement + 7 XNYS sessions``; daily short
volume ``posted_by = trade date 18:00 America/New_York``. One polite client paces every request (default
4/s, never above 5/s), retries 429/5xx/transport errors with backoff and sends a descriptive user agent
without any contact address. A rerun verifies retained files (sha256 over the raw bytes) instead of
requesting them again; absent files older than 14 days are not re-probed.

Run under the memory guard: ``run_memory_guarded.py --job-gb 0.3 --wait-minutes 30 -- python
scripts/fetch_finra_short_data.py``.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import time
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.clock import utc_today
from atx_db.connection import resolve_data_dir
from atx_db.finra import (
    FINRA_DEFAULT_REQUESTS_PER_SECOND,
    PoliteFinraClient,
    fetch_short_interest_history,
)
from atx_db.short_volume import REGSHO_DAILY_PREFIXES, fetch_regsho_daily_files

SOURCES = ("si-api", "si-cdn", "si-archive", "regsho")
#: The consolidated file and the Chicago TRF only exist from 2018-08-01; the other facilities from 2009-08-03.
LATE_PREFIXES = frozenset({"CNMS", "FNQC"})


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out-dir", type=Path, default=None, help="Default: <data dir>/raw/finra.")
    parser.add_argument("--sources", default=",".join(SOURCES), help=f"Comma list of {SOURCES}.")
    parser.add_argument("--end", type=dt.date.fromisoformat, default=None, help="Default: today (UTC).")
    parser.add_argument("--si-api-start", type=dt.date.fromisoformat, default=dt.date(2017, 6, 1))
    parser.add_argument("--si-cdn-start", type=dt.date.fromisoformat, default=dt.date(2018, 1, 1))
    parser.add_argument("--regsho-start", type=dt.date.fromisoformat, default=dt.date(2009, 7, 1))
    parser.add_argument("--regsho-late-start", type=dt.date.fromisoformat, default=dt.date(2018, 1, 2),
                        help="Probe start for CNMS and FNQC.")
    parser.add_argument("--regsho-prefixes", default=",".join(REGSHO_DAILY_PREFIXES))
    parser.add_argument("--rps", type=float, default=FINRA_DEFAULT_REQUESTS_PER_SECOND)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--absent-final-days", type=int, default=14)
    parser.add_argument("--progress-every", type=int, default=500)
    parser.add_argument("--summary-json", type=Path, default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    sources = {value.strip() for value in args.sources.split(",") if value.strip()}
    unknown = sorted(sources - set(SOURCES))
    if unknown:
        raise SystemExit(f"unknown --sources {unknown}; choose from {SOURCES}")
    end = args.end or utc_today()
    out_dir = args.out_dir or resolve_data_dir() / "raw" / "finra"
    client = PoliteFinraClient(requests_per_second=args.rps, timeout=args.timeout)
    started = time.perf_counter()

    def progress(source: str, index: int, total: int, record: dict[str, Any]) -> None:
        if index == total or index % max(args.progress_every, 1) == 0 or record.get("outcome") == "failed":
            print(json.dumps({
                "source": source, "i": index, "n": total, "key": record.get("key"),
                "outcome": record.get("outcome"), "requests": client.requests,
                "elapsed_s": round(time.perf_counter() - started, 1),
            }), flush=True)

    summary: dict[str, Any] = {"end": end.isoformat(), "out_dir": str(out_dir), "sources": sorted(sources)}
    try:
        si_sources = sources & {"si-api", "si-cdn", "si-archive"}
        if si_sources:
            summary["short_interest"] = fetch_short_interest_history(
                client,
                out_dir / "short-interest",
                api_start=args.si_api_start,
                cdn_start=args.si_cdn_start,
                end=end,
                include_api="si-api" in sources,
                include_cdn="si-cdn" in sources,
                include_archive="si-archive" in sources,
                absent_final_days=args.absent_final_days,
                progress=progress,
            )
        if "regsho" in sources:
            prefixes = [value.strip().upper() for value in args.regsho_prefixes.split(",") if value.strip()]
            summary["regsho_daily"] = fetch_regsho_daily_files(
                client,
                out_dir / "regsho-daily",
                prefixes=prefixes,
                start_by_prefix={
                    prefix: args.regsho_late_start if prefix in LATE_PREFIXES else args.regsho_start
                    for prefix in prefixes
                },
                end=end,
                absent_final_days=args.absent_final_days,
                progress=progress,
            )
    finally:
        summary["client"] = client.stats()
        summary["elapsed_s"] = round(time.perf_counter() - started, 1)
        client.close()
        text = json.dumps(summary, indent=2, default=str)
        if args.summary_json is not None:
            args.summary_json.parent.mkdir(parents=True, exist_ok=True)
            args.summary_json.write_text(text, encoding="utf-8")
        print(text, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
