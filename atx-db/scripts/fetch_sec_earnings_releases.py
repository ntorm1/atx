"""Fetch worker for SEC 8-K Item 2.02 EX-99 earnings releases (fetch half of the fetch/load split).

Candidates come straight from the retained bulk ``submissions.zip`` (no warehouse, no DuckDB
connection). Each candidate's filing index and its single EX-99 go into a ``sec_http`` fetch
store under ``--fetch-dir``: content-addressed files plus ``fetch-ledger.jsonl``. Every request
sends only the approved SEC user agent and takes a token from the host-wide 5 req/s limiter
(one fixed lock per host, whatever root the worker is launched from), so several workers
(``--shard K/N``) may run at once. A rerun skips every ledgered URL, so a killed run resumes
where it stopped. Load afterwards with
``scripts/refresh_sec_earnings_release_facts.py --fetch-dir`` (loader-only, no network).

Run each worker under the memory guard (ruling C-58): ``run_memory_guarded.py --job-gb 0.6
--wait-minutes 30 -- python scripts/fetch_sec_earnings_releases.py ...``.

Exit codes: 0 done; 3 SEC blocked the host (403/429 on every retry, see ``sec_http``), or the
host was already tripped at start: nothing more is requested until an operator runs
``python -m atx_db.sec_http --clear-block``. A worker that cannot reach the host-wide limiter
lock refuses to start.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.connection import resolve_data_dir  # noqa: E402
from atx_db.press_release import (  # noqa: E402
    earnings_release_candidates_from_submissions_archive,
    fetch_sec_earnings_release_documents,
)
from atx_db.sec_http import SecBlockedError, default_sec_limiter

EXIT_SEC_BLOCKED = 3


def _shard(value: str) -> tuple[int, int]:
    index, _, count = value.partition("/")
    shard, shards = int(index), int(count)
    if shards < 1 or not 0 <= shard < shards:
        raise argparse.ArgumentTypeError("--shard must be K/N with 0 <= K < N")
    return shard, shards


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fetch-dir", type=Path, required=True)
    parser.add_argument("--submissions-zip", type=Path, default=resolve_data_dir() / "cache" / "submissions.zip")
    parser.add_argument("--cik", action="append", default=[], help="Repeatable; default: every CIK in the archive.")
    parser.add_argument("--history-start", type=dt.date.fromisoformat)
    parser.add_argument("--history-end", type=dt.date.fromisoformat)
    parser.add_argument("--shard", type=_shard, default=(0, 1), help="K/N: this worker's slice of the candidates.")
    parser.add_argument("--request-timeout", type=float, default=30.0)
    parser.add_argument("--max-index-bytes", type=int, default=2_000_000)
    parser.add_argument("--max-document-bytes", type=int, default=8_000_000)
    parser.add_argument("--progress-every", type=int, default=10)
    args = parser.parse_args()

    shard, shards = args.shard
    started = time.monotonic()
    # Refuses to start on an ATX_SEC_RATE_LOCK naming a second lock, an unreachable host lock, or a tripped host.
    limiter = default_sec_limiter()
    try:
        limiter_state = limiter.preflight()
    except SecBlockedError as exc:
        print(json.dumps({"aborted": "sec_blocked_at_start", "pid": os.getpid(), "error": str(exc)}), flush=True)
        return EXIT_SEC_BLOCKED
    print(json.dumps({"start": {"pid": os.getpid(), "shard": f"{shard}/{shards}", "limiter": limiter_state}},
                     sort_keys=True), flush=True)
    candidates = (
        candidate
        for position, candidate in enumerate(earnings_release_candidates_from_submissions_archive(
            args.submissions_zip,
            ciks=tuple(args.cik) or None,
            history_start=args.history_start,
            history_end=args.history_end,
        ))
        if position % shards == shard
    )
    counts: dict[str, int] = {}

    def progress(snapshot: dict[str, int]) -> None:
        counts.update(snapshot)
        print(json.dumps({"progress": snapshot, "pid": os.getpid(), "elapsed_s": round(time.monotonic() - started, 1)}),
              flush=True)

    try:
        counts = fetch_sec_earnings_release_documents(
            candidates,
            args.fetch_dir,
            limiter=limiter,
            request_timeout=args.request_timeout,
            max_index_bytes=args.max_index_bytes,
            max_document_bytes=args.max_document_bytes,
            progress=progress,
            progress_every=args.progress_every,
        )
    except SecBlockedError as exc:
        print(json.dumps({"aborted": "sec_blocked", "pid": os.getpid(), "shard": f"{shard}/{shards}",
                          "last_progress": counts, "error": str(exc),
                          "elapsed_s": round(time.monotonic() - started, 1)}, sort_keys=True), flush=True)
        return EXIT_SEC_BLOCKED
    print(json.dumps({"done": counts, "pid": os.getpid(), "shard": f"{shard}/{shards}",
                      "elapsed_s": round(time.monotonic() - started, 1)}, sort_keys=True), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
