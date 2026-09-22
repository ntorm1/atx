"""Idempotent, resumable, deterministic full-warehouse activation.

The ladder runs one stage at a time in dependency order, records every attempt in
``activation_stage_runs``, and skips a stage whose newest attempt completed unless
``force`` is set. Every stage is a pure ``(store, options) -> StageResult``; the
ladder owns all ledgering, ordering, and reporting so a stage function stays
testable on its own.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import logging
import os
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Protocol

import pandas as pd

from .clock import resolve_as_of_date, utc_today
from .connection import DEFAULT_DB_PATH, DuckDBStore
from .security_master import SEC_COMPANY_TICKERS_URL, normalize_company_tickers, upsert_security_master_from_frame
from .ticker_history_bulk import BulkTickerHistoryOptions, publish_bulk_ticker_history
from .ticker_history_extract import (
    TICKER_HISTORY_MEMBER,
    TICKER_HISTORY_UNCOMPRESSED_BYTES,
    extract_ticker_history_tsv,
)
from .warehouse import file_sha256, now_utc_naive, record_source_file

LOGGER = logging.getLogger(__name__)

COMPANYFACTS_ZIP_URL = "https://www.sec.gov/Archives/edgar/daily-index/xbrl/companyfacts.zip"
SUBMISSIONS_ZIP_URL = "https://www.sec.gov/Archives/edgar/daily-index/bulkdata/submissions.zip"
NASDAQ_LISTED_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt"
OTHER_LISTED_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt"
DEFAULT_TICKER_HISTORY_ZIP = Path.home() / "Downloads" / "tbltickerhistory3_10y.zip"

STAGE_ORDER: tuple[str, ...] = (
    "migrate",
    "security_master",
    "symbol_directory",
    "ticker_history_extract",
    "ticker_history_publish",
    "sec_bulk_download",
    "submissions_load",
    "earnings_release_facts",
    "companyfacts_load",
    "statement_points",
    "periods",
    "ttm",
    "calendarization",
    "standardized",
    "industry_templates",
    "reconciliation",
    "derived_metrics",
    "market_daily",
    "legacy_liquid_universe",
    "factor_projections",
    "delisting_evidence",
    "universe_us_listed",
    "delisting_terminal_returns",
    "trading_calendar",
    "survivorship_forward_returns",
    "item_coverage",
    "provider_coverage",
    "equity_price_metrics",
    "quality",
)


@dataclass(frozen=True)
class StageResult:
    """What a stage did: a row count plus a JSON-serialisable detail payload."""

    rows: int
    detail: dict[str, object]


class ActivationStageError(RuntimeError):
    """A failed stage with committed partial work and inspectable outcome counts."""

    def __init__(self, message: str, result: StageResult) -> None:
        self.result = result
        super().__init__(f"{message}: {json.dumps(result.detail, default=str, sort_keys=True)}")


def select_stages(
    *,
    start: str | None = None,
    stop: str | None = None,
    only: tuple[str, ...] = (),
) -> tuple[str, ...]:
    """Resolve a ladder slice, always in ``STAGE_ORDER`` order."""
    for name in (*(() if start is None else (start,)), *(() if stop is None else (stop,)), *only):
        if name not in STAGE_ORDER:
            raise ValueError(f"unknown activation stage {name!r}; expected one of {STAGE_ORDER}")
    if only:
        chosen = set(only)
        return tuple(stage for stage in STAGE_ORDER if stage in chosen)
    first = 0 if start is None else STAGE_ORDER.index(start)
    last = len(STAGE_ORDER) - 1 if stop is None else STAGE_ORDER.index(stop)
    if last < first:
        raise ValueError(f"--stop-stage {stop!r} precedes --start-stage {start!r}")
    return STAGE_ORDER[first : last + 1]


def begin_stage(
    store: DuckDBStore,
    *,
    stage: str,
    run_id: str,
    params: dict[str, object],
) -> dt.datetime:
    """Record a ``running`` attempt and return its start stamp."""
    started_at = now_utc_naive()
    store.con.execute(
        """
        INSERT OR REPLACE INTO activation_stage_runs (
            stage, run_id, status, started_at, finished_at, "rows", params_json, error
        ) VALUES (?, ?, 'running', ?, NULL, NULL, ?, NULL)
        """,
        [stage, run_id, started_at, json.dumps(params, default=str, sort_keys=True)],
    )
    return started_at


def finish_stage(
    store: DuckDBStore,
    *,
    stage: str,
    run_id: str,
    started_at: dt.datetime,
    status: str,
    rows: int = 0,
    error: str | None = None,
) -> None:
    """Close out an attempt as ``completed``, ``failed``, ``skipped`` or ``dry_run``."""
    store.con.execute(
        """
        UPDATE activation_stage_runs
        SET status = ?, finished_at = ?, "rows" = ?, error = ?
        WHERE stage = ? AND run_id = ?
        """,
        [status, now_utc_naive(), int(rows), error, stage, run_id],
    )
    _ = started_at  # start stamp is already persisted by begin_stage


def completed_stages(store: DuckDBStore) -> set[str]:
    """Return the stages whose NEWEST attempt completed.

    A later failure supersedes an earlier success and vice versa, so a resumed
    ladder always reflects the most recent evidence.
    """
    rows = store.con.execute(
        """
        SELECT stage
        FROM (
            SELECT stage, status,
                   row_number() OVER (
                       PARTITION BY stage ORDER BY started_at DESC, run_id DESC
                   ) AS attempt_rank
            FROM activation_stage_runs
        )
        WHERE attempt_rank = 1 AND status = 'completed'
        ORDER BY stage
        """
    ).fetchall()
    return {stage for (stage,) in rows}


class Downloader(Protocol):
    """Fetch ``url`` to ``dest`` and return the number of bytes on disk.

    The only network seam in the activation ladder. Real runs pass
    ``requests_downloader``; tests pass a fake that writes a fixture payload.
    """

    def __call__(self, url: str, dest: Path, *, user_agent: str) -> int: ...


class ShardRunner(Protocol):
    """Run the sharded reconciliation publish and return its captured result.

    The only subprocess seam in the activation ladder. Real runs pass
    ``run_reconciliation_shards_subprocess``; tests pass a fake that returns a
    canned ``CompletedProcess`` without spawning anything.
    """

    def __call__(self, argv: list[str]) -> subprocess.CompletedProcess[str]: ...


@dataclass(frozen=True)
class ActivationOptions:
    db_path: Path = DEFAULT_DB_PATH
    as_of_date: dt.date | None = None
    ticker_history_zip: Path = DEFAULT_TICKER_HISTORY_ZIP
    ticker_history_source_path: Path | None = None
    ticker_history_expected_bytes: int | None = TICKER_HISTORY_UNCOMPRESSED_BYTES
    staging_dir: Path = Path("data/staging/broad-bars")
    cache_dir: Path = Path("data/cache")
    sec_user_agent: str | None = None
    downloader: Downloader | None = None
    memory_limit: str = "1GB"
    threads: int = 1
    reconciliation_shards: int = 16
    shard_runner: ShardRunner | None = None
    minimum_rows: int = 30_000_000
    minimum_securities: int = 10_000
    minimum_latest_date_securities: int = 5_000
    companyfacts_limit: int | None = None
    companyfacts_progress_every: int = 25
    companyfacts_resume_from_run_id: str | None = None
    submissions_batch_size: int = 50
    submissions_resume_from_run_id: str | None = None
    earnings_release_cache_dir: Path | None = None
    earnings_release_history_start: dt.date | None = None
    earnings_release_history_end: dt.date | None = None
    earnings_release_ciks: tuple[str, ...] | None = None
    earnings_release_request_timeout: float = 30.0
    earnings_release_max_index_bytes: int = 2_000_000
    earnings_release_max_document_bytes: int = 8_000_000
    earnings_release_candidate_batch_size: int = 250
    companyfacts_symbol_source: str = "sec_company_tickers"
    skip_loaded_companyfacts: bool | None = None
    dry_run: bool = False
    force: bool = False
    run_id: str = "warehouse-activate"

    def __post_init__(self) -> None:
        if min(self.threads, self.reconciliation_shards, self.submissions_batch_size,
               self.earnings_release_candidate_batch_size, self.earnings_release_max_index_bytes,
               self.earnings_release_max_document_bytes) < 1 or self.earnings_release_request_timeout <= 0:
            raise ValueError("threads, reconciliation_shards and submissions_batch_size must be positive")
        if (self.earnings_release_history_start is not None and self.earnings_release_history_end is not None
                and self.earnings_release_history_end < self.earnings_release_history_start):
            raise ValueError("earnings-release history end precedes start")
        if not self.memory_limit.strip():
            raise ValueError("memory_limit must not be empty")

    def as_dict(self) -> dict[str, object]:
        """Shallow field mapping for ``ActivationOptions(**{**opts.as_dict(), ...})``."""
        return dict(asdict(self)) | {"downloader": self.downloader, "shard_runner": self.shard_runner}

    def ledger_params(self) -> dict[str, object]:
        """JSON-safe option snapshot for ``activation_stage_runs.params_json``."""
        payload = {
            key: value for key, value in self.as_dict().items() if key not in ("downloader", "shard_runner")
        }
        return {key: (str(value) if isinstance(value, Path) else value) for key, value in payload.items()}

    @property
    def tsv_path(self) -> Path:
        return Path(self.staging_dir) / TICKER_HISTORY_MEMBER

    @property
    def companyfacts_zip(self) -> Path:
        return Path(self.cache_dir) / "companyfacts.zip"

    @property
    def submissions_zip(self) -> Path:
        return Path(self.cache_dir) / "submissions.zip"


def require_sec_user_agent(options: ActivationOptions) -> str:
    """Return the SEC user agent or fail fast with an operator-actionable message."""
    agent = options.sec_user_agent or os.environ.get("ATX_SEC_USER_AGENT")
    if not agent or not agent.strip():
        raise ValueError(
            "ATX_SEC_USER_AGENT is required before any SEC request. Set it to a product "
            'name and a monitored contact address, e.g. ATX_SEC_USER_AGENT="atx-db/0.2 '
            'ops@example.com", or pass --sec-user-agent.'
        )
    return agent.strip()


def stage_migrate(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Bring the warehouse to head schema and assert it got there."""
    from .migrations import MIGRATIONS, apply_pending_migrations

    applied = apply_pending_migrations(store.con)
    row = store.con.execute(
        "SELECT max(try_cast(version AS INTEGER)) FROM schema_migrations"
    ).fetchone()
    if row is None or row[0] is None:
        raise RuntimeError("warehouse has no schema_migrations rows after migrate")
    version = int(row[0])
    target = max(migration.version for migration in MIGRATIONS)
    if version != target:
        raise RuntimeError(f"warehouse is at schema {version}, expected head {target}")
    _ = options
    return StageResult(
        rows=len(applied),
        detail={"schema_version": version, "applied_versions": list(applied)},
    )


def stage_security_master(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Load SEC ``company_tickers.json`` into ``sec_company_tickers`` (network)."""
    user_agent = require_sec_user_agent(options)
    download = _require_downloader(options)
    cache_path = Path(options.cache_dir) / "company_tickers.json"
    byte_count = download(SEC_COMPANY_TICKERS_URL, cache_path, user_agent=user_agent)
    frame = normalize_company_tickers(json.loads(cache_path.read_text(encoding="utf-8")))
    as_of_date = resolve_as_of_date(options.as_of_date, source_max_date=None)
    upsert_security_master_from_frame(
        store,
        frame,
        source="SEC company_tickers",
        as_of_date=as_of_date,
        run_id=f"{options.run_id}-security-master",
    )
    checksum = sha256_file(cache_path)
    record_source_file(
        store,
        dataset_id="sec_security_master",
        source_url=SEC_COMPANY_TICKERS_URL,
        cache_path=cache_path,
        status="available",
        sha256=checksum,
        metadata={"as_of_date": as_of_date.isoformat(), "bytes": byte_count, "sha256": checksum},
    )
    count = store.con.execute("SELECT count(*) FROM sec_company_tickers").fetchone()
    return StageResult(
        rows=0 if count is None else int(count[0]),
        detail={"as_of_date": as_of_date.isoformat(), "bytes": byte_count},
    )


def stage_symbol_directory(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Load the Nasdaq Trader symbol directory snapshot (network)."""
    from .symbol_directory import (
        NasdaqSymbolDirectoryOptions,
        _read_directory_text,
        normalize_nasdaq_listed,
        normalize_other_listed,
        resolve_directory_as_of_date,
    )
    from .warehouse import insert_frame

    download = _require_downloader(options)
    user_agent = require_sec_user_agent(options)
    directory_options = NasdaqSymbolDirectoryOptions(as_of_date=options.as_of_date)
    texts: list[tuple[str, str, Callable[..., pd.DataFrame]]] = []
    fetched: list[tuple[str, Path]] = []
    for url, normalizer, name in (
        (NASDAQ_LISTED_URL, normalize_nasdaq_listed, "nasdaqlisted.txt"),
        (OTHER_LISTED_URL, normalize_other_listed, "otherlisted.txt"),
    ):
        dest = Path(options.cache_dir) / name
        download(url, dest, user_agent=user_agent)
        texts.append((url, dest.read_text(encoding="utf-8"), normalizer))
        fetched.append((url, dest))
    as_of_date = resolve_directory_as_of_date(directory_options, texts[0][1])
    for url, dest in fetched:
        checksum = sha256_file(dest)
        record_source_file(
            store,
            dataset_id="nasdaq_symbol_directory",
            source_url=url,
            cache_path=dest,
            status="available",
            sha256=checksum,
            metadata={"as_of_date": as_of_date.isoformat(), "bytes": dest.stat().st_size, "sha256": checksum},
        )
    frames = [
        normalizer(
            _read_directory_text(text),
            as_of_date=as_of_date,
            source_url=url,
            run_id=f"{options.run_id}-symbol-directory",
        )
        for url, text, normalizer in texts
    ]
    frame = pd.concat([f for f in frames if not f.empty], ignore_index=True)
    with store.transaction():
        store.con.execute("DELETE FROM nasdaq_symbol_directory WHERE as_of_date = ?", [as_of_date])
        insert_frame(store, frame, "nasdaq_symbol_directory", "activation_symbol_directory_insert")
    return StageResult(
        rows=len(frame),
        detail={"as_of_date": as_of_date.isoformat(), "symbols": int(frame["symbol"].nunique())},
    )


def stage_ticker_history_extract(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Stream the ticker-history ZIP member into the staging TSV (offline)."""
    _ = store
    if options.ticker_history_source_path is not None:
        path = options.ticker_history_source_path.resolve()
        if not path.is_file():
            raise FileNotFoundError(path)
        return StageResult(rows=0, detail={"source_path": str(path), "skipped": True,
                                         "reason": "native source supplied; no extraction required"})
    result = extract_ticker_history_tsv(
        Path(options.ticker_history_zip),
        options.tsv_path,
        expected_bytes=options.ticker_history_expected_bytes,
        progress=lambda written, total: LOGGER.info(
            "ticker-history extract %.1f%% (%s / %s bytes)",
            100.0 * written / total,
            f"{written:,}",
            f"{total:,}",
        ),
    )
    return StageResult(
        rows=result.written_bytes,
        detail={
            "tsv_path": str(result.tsv_path),
            "member_name": result.member_name,
            "expected_bytes": result.expected_bytes,
            "written_bytes": result.written_bytes,
            "sha256": result.sha256,
            "skipped": result.skipped,
        },
    )


def stage_ticker_history_publish(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Atomically publish the staging TSV into ``equity_daily_bars`` (offline)."""
    result = publish_bulk_ticker_history(
        store,
        BulkTickerHistoryOptions(
            source_path=(options.ticker_history_source_path or options.tsv_path).resolve(),
            memory_limit=options.memory_limit,
            threads=options.threads,
            minimum_rows=options.minimum_rows,
            minimum_securities=options.minimum_securities,
            minimum_latest_date_securities=options.minimum_latest_date_securities,
            run_id=f"{options.run_id}-broad-bars",
        ),
    )
    return StageResult(
        rows=result.rows,
        detail={
            "securities": result.securities,
            "latest_date": result.latest_date.isoformat(),
            "latest_date_securities": result.latest_date_securities,
            "invalid_rows": result.invalid_rows,
            "duplicate_keys": result.duplicate_keys,
            "elapsed_seconds": round(result.elapsed_seconds, 3),
            "source_diagnostics": result.source_diagnostics,
            "provenance": result.provenance,
        },
    )


def _require_downloader(options: ActivationOptions) -> Downloader:
    if options.downloader is None:
        raise ValueError(
            "a Downloader is required for network stages; the CLI injects "
            "requests_downloader and tests inject a fake"
        )
    return options.downloader


def sha256_file(path: Path, *, chunk_bytes: int = 1 << 22) -> str:
    """Stream a file through sha256 without loading it into memory."""
    return file_sha256(path, chunk_size=chunk_bytes)


def requests_downloader(url: str, dest: Path, *, user_agent: str) -> int:
    """Resumable streaming HTTP download. Network — never called from tests."""
    import requests

    dest.parent.mkdir(parents=True, exist_ok=True)
    partial = dest.with_suffix(dest.suffix + ".part")
    existing = partial.stat().st_size if partial.is_file() else 0
    headers = {"User-Agent": user_agent, "Accept-Encoding": "identity"}
    if existing:
        headers["Range"] = f"bytes={existing}-"
    with requests.get(url, headers=headers, stream=True, timeout=600) as response:
        if existing and response.status_code == 200:
            existing = 0  # server ignored the range request; restart
        response.raise_for_status()
        mode = "ab" if existing else "wb"
        with open(partial, mode) as handle:
            for chunk in response.iter_content(chunk_size=1 << 20):
                if chunk:
                    handle.write(chunk)
    partial.replace(dest)
    return dest.stat().st_size


def _download_archive(
    store: DuckDBStore,
    options: ActivationOptions,
    *,
    url: str,
    dest: Path,
    dataset_id: str,
    user_agent: str,
) -> tuple[str, int, bool]:
    """Fetch ``url`` to ``dest`` unless it is already present; return (sha256, bytes, skipped)."""
    skipped = dest.is_file() and dest.stat().st_size > 0
    if not skipped:
        _require_downloader(options)(url, dest, user_agent=user_agent)
    checksum = sha256_file(dest)
    record_source_file(
        store,
        dataset_id=dataset_id,
        source_url=url,
        cache_path=dest,
        status="available",
        sha256=checksum,
        metadata={"sha256": checksum, "bytes": dest.stat().st_size, "resumed": skipped},
        compute_hash=False,
    )
    return checksum, int(dest.stat().st_size), skipped


def stage_sec_bulk_download(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Fetch companyfacts.zip and submissions.zip with resume + recorded sha256 (network)."""
    user_agent = require_sec_user_agent(options)
    facts_sha, facts_bytes, facts_skipped = _download_archive(
        store,
        options,
        url=COMPANYFACTS_ZIP_URL,
        dest=options.companyfacts_zip,
        dataset_id="sec_company_facts",
        user_agent=user_agent,
    )
    subs_sha, subs_bytes, subs_skipped = _download_archive(
        store,
        options,
        url=SUBMISSIONS_ZIP_URL,
        dest=options.submissions_zip,
        dataset_id="sec_submissions",
        user_agent=user_agent,
    )
    return StageResult(
        rows=2,
        detail={
            "companyfacts_path": str(options.companyfacts_zip),
            "companyfacts_sha256": facts_sha,
            "companyfacts_bytes": facts_bytes,
            "companyfacts_skipped": facts_skipped,
            "submissions_path": str(options.submissions_zip),
            "submissions_sha256": subs_sha,
            "submissions_bytes": subs_bytes,
            "submissions_skipped": subs_skipped,
        },
    )


def stage_submissions_load(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Load SEC filing histories from the bulk submissions archive (offline)."""
    from .sec_submissions import SecSubmissionsBulkDataset, SecSubmissionsBulkOptions

    if not options.submissions_zip.is_file():
        raise FileNotFoundError(options.submissions_zip)
    result = SecSubmissionsBulkDataset().run(
        store,
        SecSubmissionsBulkOptions(
            zip_path=options.submissions_zip,
            forms=None,
            batch_ciks=options.submissions_batch_size,
            resume_from_run_id=options.submissions_resume_from_run_id,
            run_id=f"{options.run_id}-submissions",
        ),
    )
    if result.details.get("missing_history_members", 0):
        raise ValueError("SEC submissions archive is missing referenced history; full stage is incomplete")
    return StageResult(rows=int(result.rows_loaded), detail={**result.details, "forms": "all",
                                                           "batch_size": options.submissions_batch_size})


def stage_earnings_release_facts(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Load governed Item 2.02 EX-99 source evidence after submissions."""
    from .press_release import SecEarningsReleaseDataset, SecEarningsReleaseOptions

    result = SecEarningsReleaseDataset().run(store, SecEarningsReleaseOptions(
        cache_dir=options.earnings_release_cache_dir or Path(options.cache_dir) / "sec-earnings-release",
        history_start=options.earnings_release_history_start,
        history_end=options.earnings_release_history_end,
        ciks=options.earnings_release_ciks,
        request_timeout=options.earnings_release_request_timeout,
        max_index_bytes=options.earnings_release_max_index_bytes,
        max_document_bytes=options.earnings_release_max_document_bytes,
        candidate_batch_size=options.earnings_release_candidate_batch_size,
        user_agent="atx-db/0.1 atx-research@example.com",
        run_id=f"{options.run_id}-earnings-release",
    ))
    return StageResult(rows=int(result.rows_loaded), detail=dict(result.details))


def stage_companyfacts_load(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Load the selected local CIK corpus; archive mode replaces every selected CIK."""
    from .fundamentals import SecCompanyFactsDataset, SecCompanyFactsOptions

    if not options.companyfacts_zip.is_file():
        raise FileNotFoundError(options.companyfacts_zip)
    skip_loaded = options.skip_loaded_companyfacts
    if skip_loaded is None:
        skip_loaded = options.companyfacts_symbol_source != "archive_members"
    fact_options = SecCompanyFactsOptions(
        symbols=(),
        symbol_source=options.companyfacts_symbol_source,
        symbol_limit=options.companyfacts_limit,
        skip_loaded_targets=skip_loaded,
        resume_from_run_id=options.companyfacts_resume_from_run_id,
        user_agent=options.sec_user_agent or "",
        skip_failed_targets=True,
        companyfacts_zip=options.companyfacts_zip,
        as_of_date=options.as_of_date,
        refresh_derived_surfaces=False,
        progress_every_targets=options.companyfacts_progress_every,
        run_id=f"{options.run_id}-companyfacts",
    )
    result = SecCompanyFactsDataset().run(store, fact_options)
    detail = dict(result.details)
    detail["skip_loaded"] = skip_loaded
    stage_result = StageResult(rows=int(result.rows_loaded), detail=detail)
    if detail["failed_target_count"] or detail["outcome"] == "no_valid_targets":
        raise ActivationStageError("companyfacts ingestion incomplete", stage_result)
    return stage_result


STAGES: dict[str, Callable[[DuckDBStore, ActivationOptions], StageResult]] = {
    "migrate": stage_migrate,
    "security_master": stage_security_master,
    "symbol_directory": stage_symbol_directory,
    "ticker_history_extract": stage_ticker_history_extract,
    "ticker_history_publish": stage_ticker_history_publish,
}

STAGES.update(
    {
        "sec_bulk_download": stage_sec_bulk_download,
        "submissions_load": stage_submissions_load,
        "earnings_release_facts": stage_earnings_release_facts,
        "companyfacts_load": stage_companyfacts_load,
    }
)


def stage_statement_points(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Rebuild effective revisions, statement points, and dependent SEC shares."""
    from ._fundamental_clock import FUNDAMENTAL_CLOCK_POLICY
    from .fundamental_statements import refresh_fundamental_statement_points
    from .fundamentals import refresh_fundamental_fact_revisions, refresh_xbrl_concept_catalog
    from .shares_outstanding import SharesOutstandingHistoryOptions, refresh_shares_outstanding_history

    catalog_rows = refresh_xbrl_concept_catalog(store)
    revision_rows = refresh_fundamental_fact_revisions(store)
    point_rows = refresh_fundamental_statement_points(store)
    share_rows = refresh_shares_outstanding_history(
        store, SharesOutstandingHistoryOptions(run_id=f"{options.run_id}-shares")
    )
    return StageResult(
        rows=int(point_rows),
        detail={
            "catalog_rows": int(catalog_rows),
            "revision_rows": int(revision_rows),
            "statement_point_rows": int(point_rows),
            "shares_history_rows": int(share_rows),
            "fundamental_clock_policy": FUNDAMENTAL_CLOCK_POLICY,
        },
    )


def stage_periods(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .fundamental_statements import refresh_fundamental_periods

    _ = options
    rows = refresh_fundamental_periods(store)
    return StageResult(rows=int(rows), detail={"period_rows": int(rows)})


def stage_ttm(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .fundamental_statements import refresh_fundamental_ttm_points

    _ = options
    rows = refresh_fundamental_ttm_points(store)
    return StageResult(rows=int(rows), detail={"ttm_rows": int(rows)})


def stage_calendarization(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .calendarization import CalendarizationOptions, run_calendarization_refresh

    summary = run_calendarization_refresh(
        store,
        CalendarizationOptions(
            run_id=f"{options.run_id}-calendarization", as_of_date=options.as_of_date
        ),
    )
    rows = int(summary.get("calendar_ttm_rows", summary.get("map_rows", 0)) or 0)
    return StageResult(rows=rows, detail=dict(summary))


def stage_standardized(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .standardization import FundamentalStandardizationOptions, refresh_fundamental_standardized

    # memory_limit/threads/preserve_insertion_order are configured once by
    # run_activation right after it opens the store (and replayed by
    # DuckDBStore.reopen() after the reconciliation stage's close/reopen), not
    # per stage -- this used to call _configure_analytical_session itself.
    result = refresh_fundamental_standardized(
        store,
        FundamentalStandardizationOptions(
            materialize_result_limit=0,
            run_id=f"{options.run_id}-standardized",
        ),
    )
    return StageResult(
        rows=int(result.standardized_row_count),
        detail={
            "build_id": result.build_id,
            "rule_set_sha256": result.rule_set_sha256,
            "input_rows": int(result.input_row_count),
            "exception_rows": int(result.exception_row_count),
            "basis_counts": dict(result.basis_counts or {}),
        },
    )


def stage_industry_templates(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .industry_templates import IndustryTemplateOptions, run_industry_template_refresh

    summary = run_industry_template_refresh(
        store, IndustryTemplateOptions(run_id=f"{options.run_id}-industry")
    )
    return StageResult(rows=int(summary.get("coverage_rows", 0) or 0), detail=dict(summary))


def run_reconciliation_shards_subprocess(argv: list[str]) -> subprocess.CompletedProcess[str]:
    """Default ``ShardRunner``: spawn the sharded script and capture its output.

    Never called from tests -- tests inject a fake ``ShardRunner`` that returns a
    canned ``CompletedProcess`` without spawning anything.
    """
    return subprocess.run(argv, check=False, capture_output=True, text=True)


def _parse_shard_json_lines(stdout: str) -> list[dict[str, object]]:
    """Best-effort parse of the shard child's one-JSON-object-per-line stdout.

    A line that isn't a JSON object is skipped rather than raising: the child is
    a separate process, and nothing it prints may break the ladder's own
    one-JSON-line-per-stage contract on our own stdout.
    """
    events: list[dict[str, object]] = []
    for raw_line in stdout.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(payload, dict):
            events.append(payload)
    return events


def _reconciliation_script_path() -> Path:
    return Path(__file__).resolve().parents[2] / "scripts" / "refresh_reconciliation_sharded.py"


def stage_reconciliation(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Publish the reconciliation serving table in bounded symbol shards.

    Side effect: this stage CLOSES the ladder's connection and shells out to
    ``scripts/refresh_reconciliation_sharded.py`` (by default; ``options.shard_runner``
    overrides this, and tests always inject a fake), which runs each shard in a
    fresh interpreter. A long-lived process was measured to degrade a shard from
    ~110s to ~840s at identical warehouse size, and DuckDB allows exactly one
    writer, so the ladder must yield the file for the duration. The connection is
    reopened -- via ``finally``, even if the runner raises -- before returning, so
    the stage's contract to its caller is unchanged.

    The child's stdout is captured, not forwarded: it prints its own
    ``{"step": ...}`` JSON lines, which would otherwise interleave with the
    ladder's one-line-per-stage contract on our stdout. Those lines are parsed
    into this stage's detail instead. The child's stderr IS forwarded, so a
    genuine shard failure's traceback is still visible to the operator.
    """
    script = _reconciliation_script_path()
    if not script.is_file():
        raise FileNotFoundError(
            f"reconciliation shard script not found at {script}. stage_reconciliation shells out to "
            "scripts/refresh_reconciliation_sharded.py, which a wheel/sdist install of atx-db does not "
            "ship -- run from a source checkout, or install atx-db editable (pip install -e .) so "
            "scripts/ is present two directories above atx_db/activation.py."
        )
    argv = [
        sys.executable,
        str(script),
        "--db-path",
        str(options.db_path),
        "--shards",
        str(options.reconciliation_shards),
        "--memory-limit",
        options.memory_limit,
        "--threads",
        str(options.threads),
        "--run-id-prefix",
        f"{options.run_id}-recon",
    ]
    runner = options.shard_runner or run_reconciliation_shards_subprocess
    store.close()
    try:
        completed = runner(argv)
    finally:
        store.reopen()
    if completed.stderr:
        sys.stderr.write(completed.stderr)
    shard_events = _parse_shard_json_lines(completed.stdout)
    if completed.returncode != 0:
        raise RuntimeError(
            f"sharded reconciliation publish failed with exit code {completed.returncode}"
        )
    row = store.con.execute("SELECT count(*) FROM fundamental_reconciliation_serving").fetchone()
    rows = 0 if row is None else int(row[0])
    return StageResult(
        rows=rows,
        detail={
            "shards": options.reconciliation_shards,
            "serving_rows": rows,
            "shard_event_count": len(shard_events),
            "last_shard_event": shard_events[-1] if shard_events else None,
        },
    )


def stage_derived_metrics(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Seed the declarative catalog and materialise every quarterly derived metric."""
    from .derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
    from .derived_registry import seed_derived_metric_definitions

    seeded = seed_derived_metric_definitions(store)
    rows = refresh_derived_metrics(store, DerivedMetricsOptions(run_id=options.run_id))
    metric_count_row = store.con.execute(
        "SELECT count(DISTINCT metric_code) FROM derived_metric_values "
        "WHERE is_latest_revision AND value_status = 'valid' "
        "AND value IS NOT NULL AND isfinite(value) AND history_status = 'event_reconstructed'"
    ).fetchone()
    metric_count = 0 if metric_count_row is None else int(metric_count_row[0])
    return StageResult(
        rows=rows,
        detail={"definitions_seeded": seeded, "metric_count": metric_count},
    )


def stage_market_daily(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Build the daily market panel from bars plus the point-in-time fundamental state."""
    from .market_daily import (
        MarketDailyOptions,
        refresh_market_daily_metrics,
        shares_reconciliation_report,
    )

    rows = refresh_market_daily_metrics(store, MarketDailyOptions(run_id=options.run_id))
    return StageResult(rows=rows, detail=dict(shares_reconciliation_report(store)))


def stage_delisting_evidence(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Build public delisting evidence and fold it into delisting_events."""

    from .delisting_evidence import (
        DelistingEvidenceOptions,
        fold_evidence_into_delisting_events,
        refresh_delisting_evidence,
    )

    evidence_options = DelistingEvidenceOptions(
        as_of_date=options.as_of_date, run_id=options.run_id
    )
    evidence = refresh_delisting_evidence(store, evidence_options)
    events = fold_evidence_into_delisting_events(store, evidence_options)
    return StageResult(rows=evidence, detail={"delisting_events": events})


def stage_universe_us_listed(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Rebuild the point-in-time US-listed universe from bars + directory + deciles."""

    from .universe_us_listed import UniverseUsListedOptions, refresh_universe_us_listed

    rows = refresh_universe_us_listed(
        store,
        UniverseUsListedOptions(as_of_date=options.as_of_date, run_id=options.run_id),
    )
    return StageResult(rows=rows, detail={"table": "universe_us_listed_membership",
                                         **listing_input_diagnostics(store, options)})


def listing_input_diagnostics(store: DuckDBStore, options: ActivationOptions) -> dict[str, object]:
    """Measure source prerequisites without treating current snapshots as history."""
    row = store.con.execute("""
        SELECT (SELECT min(trade_date) FROM equity_daily_bars),
               (SELECT max(trade_date) FROM equity_daily_bars),
               (SELECT min(as_of_date) FROM nasdaq_symbol_directory),
               (SELECT max(as_of_date) FROM nasdaq_symbol_directory),
               (SELECT count(*) FROM exchange_listings
                 WHERE (nullif(trim(exchange_code),'') IS NOT NULL OR nullif(trim(mic),'') IS NOT NULL)
                   AND available_at<=?),
               (SELECT count(DISTINCT security_id) FROM equity_daily_bars)
    """, [dt.datetime.combine(resolve_as_of_date(options.as_of_date), dt.time(22))]).fetchone()
    assert row is not None
    return {"price_start": row[0], "price_end": row[1], "directory_first_snapshot": row[2],
            "directory_latest_snapshot": row[3], "dated_exchange_evidence_rows": row[4],
            "bar_observed_security_count": row[5],
            "historical_listing_prerequisite": "missing_or_partial" if not row[4] or row[2] is None
                or (row[0] is not None and row[2] > row[0]) else "requires_coverage_measurement",
            "bar_observed_names_are_verified_us_common_equity": False}


def stage_legacy_liquid_universe(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .universe import GovernedUniverseMembershipDataset, UniverseMembershipOptions

    result = GovernedUniverseMembershipDataset().load(store, UniverseMembershipOptions(
        end_date=resolve_as_of_date(options.as_of_date), run_id=options.run_id,
    ))
    return StageResult(result.rows_loaded, {**result.details,
        "classification_note": "legacy liquidity rules; unknown classification is not historical US-listing proof"})


def stage_factor_projections(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .derived_factor_projection import FactorProjectionOptions
    from .production_panels import DerivedFactorProjectionDataset

    result = DerivedFactorProjectionDataset().load(store, FactorProjectionOptions(
        end_date=resolve_as_of_date(options.as_of_date), run_id=options.run_id,
    ))
    return StageResult(result.rows_loaded, result.details)


def _production_panel(store: DuckDBStore, options: ActivationOptions, dataset: object) -> StageResult:
    from .dataset import Dataset
    from .production_panels import ProductionPanelOptions

    assert isinstance(dataset, Dataset)
    result = dataset.load(store, ProductionPanelOptions(resolve_as_of_date(options.as_of_date), options.run_id))
    return StageResult(result.rows_loaded, result.details)


def stage_delisting_terminal_returns(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .production_panels import DelistingTerminalDataset
    return _production_panel(store, options, DelistingTerminalDataset())


def stage_trading_calendar(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .calendar import TradingCalendarDataset, TradingCalendarOptions
    result = TradingCalendarDataset().load(store, TradingCalendarOptions(run_id=options.run_id))
    return StageResult(result.rows_loaded, {**result.details, "calendar_basis": "observed price dates; not certified exchange calendar"})


def stage_survivorship_forward_returns(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .production_panels import SurvivorshipForwardDataset
    return _production_panel(store, options, SurvivorshipForwardDataset())


def stage_item_coverage(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .production_panels import ItemCoverageDataset
    return _production_panel(store, options, ItemCoverageDataset())


def stage_equity_price_metrics(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Publish bounded daily risk metrics before the final warehouse checks."""
    from .equity_price_metrics import EquityPriceMetricsOptions, refresh_equity_price_metrics

    rows = refresh_equity_price_metrics(store, EquityPriceMetricsOptions(
        as_of_date=resolve_as_of_date(options.as_of_date), run_id=options.run_id,
    ))
    diagnostic = store.con.execute("""
        SELECT count(DISTINCT security_id), min(trade_date), max(trade_date), max(available_at)
        FROM equity_price_metrics WHERE source = ?
    """, ["derived_equity_price_metrics_v1"]).fetchone()
    assert diagnostic is not None
    return StageResult(rows, {
        "source": "derived_equity_price_metrics_v1",
        "as_of_date": resolve_as_of_date(options.as_of_date),
        "run_id": options.run_id,
        "security_count": int(diagnostic[0]),
        "trade_date_start": diagnostic[1],
        "trade_date_end": diagnostic[2],
        "max_available_at": diagnostic[3],
    })


def stage_quality(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .quality import run_warehouse_quality_checks  # type: ignore[attr-defined]
    results = run_warehouse_quality_checks(
        store, checked_at=dt.datetime.combine(resolve_as_of_date(options.as_of_date), dt.time(22)),
    )
    return StageResult(len(results), {
        "counts": {status: sum(r.status == status for r in results) for status in ("passed", "failed", "warning", "skipped")},
        "failures": [{"check": r.check_name, "severity": r.severity, "observed": r.observed_value,
                      "threshold": r.threshold_value} for r in results if r.status == "failed"],
        "outcome": "degraded" if any(r.status in ("failed", "warning") for r in results) else "passed",
    })


def stage_provider_coverage(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .provider_coverage import ProviderCoverageOptions, refresh_provider_coverage

    # Library never reads the clock: derive a deterministic observed_at from the
    # ladder's own as_of_date (the script edge's only clock read) instead of
    # letting refresh_provider_coverage fall back to dt.datetime.now(). 22:00 is
    # an arbitrary but fixed end-of-day stamp so reruns for the same as_of_date
    # are reproducible.
    observed_at = (
        dt.datetime.combine(options.as_of_date, dt.time(22, 0)) if options.as_of_date is not None else None
    )
    snapshots = refresh_provider_coverage(
        store,
        ProviderCoverageOptions(run_id=f"{options.run_id}-coverage", observed_at=observed_at),
    )
    conditions = {
        condition: sum(row.condition == condition for row in snapshots)
        for condition in ("available", "degraded", "pending", "missing")
    }
    return StageResult(
        rows=len(snapshots), detail={"schema_count": len(snapshots), "conditions": conditions}
    )


STAGES.update(
    {
        "statement_points": stage_statement_points,
        "periods": stage_periods,
        "ttm": stage_ttm,
        "calendarization": stage_calendarization,
        "standardized": stage_standardized,
        "industry_templates": stage_industry_templates,
        "reconciliation": stage_reconciliation,
        "derived_metrics": stage_derived_metrics,
        "market_daily": stage_market_daily,
        "legacy_liquid_universe": stage_legacy_liquid_universe,
        "factor_projections": stage_factor_projections,
        "delisting_evidence": stage_delisting_evidence,
        "universe_us_listed": stage_universe_us_listed,
        "delisting_terminal_returns": stage_delisting_terminal_returns,
        "trading_calendar": stage_trading_calendar,
        "survivorship_forward_returns": stage_survivorship_forward_returns,
        "item_coverage": stage_item_coverage,
        "provider_coverage": stage_provider_coverage,
        "equity_price_metrics": stage_equity_price_metrics,
        "quality": stage_quality,
    }
)


def print_json(payload: dict[str, object]) -> None:
    """Emit one JSON line to stdout, flushed, so an operator can tail the ladder."""
    print(json.dumps(payload, default=str, sort_keys=True), flush=True)


def _dry_run_done(options: ActivationOptions) -> set[str]:
    """Best-effort 'already completed' set for the dry-run plan.

    ``run_activation`` must never create or migrate the warehouse file under
    ``--dry-run`` (a write-mode ``DuckDBStore.__enter__`` bootstraps/migrates the
    schema as a side effect of ``initialize()``), so this opens read-only and
    only if the file already exists. A file that exists but predates the ledger
    table (or isn't a warehouse at all yet) reports nothing completed rather than
    raising -- dry-run's job is to report a plan, not to validate the file.
    """
    if options.force or not Path(options.db_path).is_file():
        return set()
    with DuckDBStore(options.db_path, read_only=True) as store:
        try:
            return completed_stages(store)
        except Exception:
            return set()


def _validate_submissions_resume_plan(options: ActivationOptions, stages: tuple[str, ...]) -> None:
    # Download cache hits rewrite the source receipt timestamp too. Refuse the
    # combination before opening/migrating the warehouse or refreshing sources.
    if options.submissions_resume_from_run_id is not None and "sec_bulk_download" in stages:
        raise ValueError(
            "SEC submissions resume cannot include sec_bulk_download; use --only submissions_load "
            "or --start-stage submissions_load to preserve the prior archive receipt"
        )


def _validate_companyfacts_resume_plan(options: ActivationOptions, stages: tuple[str, ...]) -> None:
    if options.companyfacts_resume_from_run_id is None:
        return
    if (options.companyfacts_symbol_source != "archive_members" or options.companyfacts_limit is not None
            or options.skip_loaded_companyfacts is True):
        raise ValueError("companyfacts resume requires the full archive with replacement enabled")
    if "sec_bulk_download" in stages:
        raise ValueError(
            "companyfacts resume cannot include sec_bulk_download; use --only companyfacts_load "
            "or --start-stage companyfacts_load to preserve the prior archive"
        )


def run_activation(
    options: ActivationOptions,
    *,
    stages: tuple[str, ...] = STAGE_ORDER,
    emit: Callable[[dict[str, object]], None] = print_json,
) -> list[dict[str, object]]:
    """Run the activation ladder, ledgering every stage and stopping on the first failure."""
    for stage in stages:
        if stage not in STAGES:
            raise ValueError(f"unknown activation stage {stage!r}; expected one of {STAGE_ORDER}")
    _validate_submissions_resume_plan(options, stages)
    _validate_companyfacts_resume_plan(options, stages)
    emitted: list[dict[str, object]] = []
    if options.dry_run:
        # Never opens a write-mode store: see _dry_run_done. Nothing is created,
        # migrated, or ledgered.
        done = _dry_run_done(options)
        for stage in stages:
            payload = {
                "stage": stage,
                "status": "dry_run",
                "rows": 0,
                "seconds": 0.0,
                "detail": {"already_completed": stage in done},
            }
            emit(payload)
            emitted.append(payload)
        return emitted
    params = options.ledger_params()
    with DuckDBStore(options.db_path) as store:
        from .cli import _configure_analytical_session

        # Configured once here (and replayed by DuckDBStore.reopen() after the
        # reconciliation stage's close/reopen), not per stage.
        _configure_analytical_session(store, memory_limit=options.memory_limit, threads=options.threads)
        done = set() if options.force else completed_stages(store)
        for stage in stages:
            if stage in done:
                payload = {
                    "stage": stage,
                    "status": "skipped",
                    "rows": 0,
                    "seconds": 0.0,
                    "detail": {"reason": "already completed; pass --force to rerun"},
                }
                emit(payload)
                emitted.append(payload)
                continue
            begun = time.monotonic()
            started_at = begin_stage(store, stage=stage, run_id=options.run_id, params=params)
            try:
                result = STAGES[stage](store, options)
            except Exception as exc:  # recorded then re-raised
                partial = exc.result if isinstance(exc, ActivationStageError) else StageResult(0, {})
                ledger_detail: dict[str, object] = {}
                for recover in (False, True):
                    try:
                        if recover:
                            store.recover_failed_connection()
                        finish_stage(
                            store,
                            stage=stage,
                            run_id=options.run_id,
                            started_at=started_at,
                            status="failed",
                            rows=partial.rows,
                            error=str(exc),
                        )
                        break
                    except Exception as ledger_error:
                        if recover:
                            note = (
                                "Operator recovery required for activation_stage_runs "
                                f"stage={stage!r}, run_id={options.run_id!r}: "
                                f"could not record the original stage failure: {ledger_error}"
                            )
                            exc.add_note(note)
                            LOGGER.error("%s", note)
                            ledger_detail = {"failure_ledger": "operator_recovery_required"}
                payload = {
                    "stage": stage,
                    "status": "failed",
                    "rows": partial.rows,
                    "seconds": round(time.monotonic() - begun, 3),
                    "detail": {**partial.detail, **ledger_detail, "error": str(exc)},
                }
                emit(payload)
                emitted.append(payload)
                raise
            finish_stage(
                store,
                stage=stage,
                run_id=options.run_id,
                started_at=started_at,
                status="completed",
                rows=result.rows,
            )
            payload = {
                "stage": stage,
                "status": "completed",
                "rows": result.rows,
                "seconds": round(time.monotonic() - begun, 3),
                "detail": result.detail,
            }
            emit(payload)
            emitted.append(payload)
    return emitted


GovernedMigrationsCallable = Callable[[Path], object]
RunActivationCallable = Callable[..., list[dict[str, object]]]


def add_activation_arguments(parser: argparse.ArgumentParser) -> None:
    """Add every ``activate`` flag to ``parser``.

    Shared by ``scripts/warehouse_activate.py`` and the ``atx-db activate``
    subcommand so the two entry points can never drift apart on flags or
    defaults.
    """
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--as-of-date", type=dt.date.fromisoformat, default=None)
    parser.add_argument("--ticker-history-zip", type=Path, default=DEFAULT_TICKER_HISTORY_ZIP)
    parser.add_argument("--ticker-history-source-path", type=Path, default=None,
                        help="Native local TSV or Parquet; bypasses ZIP extraction.")
    parser.add_argument("--staging-dir", type=Path, default=Path("data/staging/broad-bars"))
    parser.add_argument("--cache-dir", type=Path, default=Path("data/cache"))
    parser.add_argument("--sec-user-agent", default=None)
    parser.add_argument("--memory-limit", default="1GB")
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--shards", type=int, default=16)
    parser.add_argument("--companyfacts-limit", type=int, default=None)
    parser.add_argument("--companyfacts-resume-from-run-id", default=None,
                        help="Verify and resume completed members of a failed or source-incomplete companyfacts dataset UUID.")
    parser.add_argument("--submissions-batch-size", type=int, default=50)
    parser.add_argument("--earnings-release-cache-dir", type=Path)
    parser.add_argument("--earnings-release-history-start", type=dt.date.fromisoformat)
    parser.add_argument("--earnings-release-history-end", type=dt.date.fromisoformat)
    parser.add_argument("--earnings-release-cik", action="append", default=[])
    parser.add_argument("--earnings-release-request-timeout", type=float, default=30.0)
    parser.add_argument("--earnings-release-max-index-bytes", type=int, default=2_000_000)
    parser.add_argument("--earnings-release-max-document-bytes", type=int, default=8_000_000)
    parser.add_argument("--earnings-release-candidate-batch-size", type=int, default=250)
    parser.add_argument("--submissions-resume-from-run-id", default=None,
                        help="Verify and resume the full archive prefix of a failed submissions dataset UUID.")
    parser.add_argument("--companyfacts-symbol-source", choices=("sec_company_tickers", "archive_members"),
                        default="sec_company_tickers", help="archive_members discovers all exact local CIK members offline.")
    companyfacts_policy = parser.add_mutually_exclusive_group()
    companyfacts_policy.add_argument("--companyfacts-append-missing", dest="skip_loaded_companyfacts",
                                    action="store_true", default=None,
                                    help="Skip CIKs with any facts; NOT archive/allowlist completion evidence.")
    companyfacts_policy.add_argument("--companyfacts-replace-existing", dest="skip_loaded_companyfacts",
                                    action="store_false", help="Replace selected CIKs (default for archive_members).")
    parser.add_argument("--start-stage", choices=STAGE_ORDER, default=None)
    parser.add_argument("--stop-stage", choices=STAGE_ORDER, default=None)
    parser.add_argument("--only", action="append", choices=STAGE_ORDER, default=None)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--run-id", default=None)
    parser.add_argument(
        "--backup-keep",
        type=int,
        default=3,
        help="Most recent pre-migrate backups to retain after a governed migration run (default: 3).",
    )


def activation_options_from_args(args: argparse.Namespace) -> ActivationOptions:
    """Resolve parsed ``add_activation_arguments`` flags into ``ActivationOptions``.

    ``as_of_date`` resolves the wall clock here, at the CLI edge, exactly once per
    invocation -- never inside library code (see ``atx_db.clock``). Shared by
    ``scripts/warehouse_activate.py`` and the ``atx-db activate`` subcommand.
    """
    as_of_date = args.as_of_date or utc_today()
    sec_user_agent = args.sec_user_agent or os.environ.get("ATX_SEC_USER_AGENT")
    run_id = args.run_id or f"warehouse-activate-{as_of_date.isoformat()}"
    return ActivationOptions(
        db_path=args.db_path,
        as_of_date=as_of_date,
        ticker_history_zip=args.ticker_history_zip,
        ticker_history_source_path=args.ticker_history_source_path,
        staging_dir=args.staging_dir,
        cache_dir=args.cache_dir,
        sec_user_agent=sec_user_agent,
        downloader=requests_downloader,
        memory_limit=args.memory_limit,
        threads=args.threads,
        reconciliation_shards=args.shards,
        companyfacts_limit=args.companyfacts_limit,
        companyfacts_resume_from_run_id=args.companyfacts_resume_from_run_id,
        submissions_batch_size=args.submissions_batch_size,
        submissions_resume_from_run_id=args.submissions_resume_from_run_id,
        earnings_release_cache_dir=args.earnings_release_cache_dir,
        earnings_release_history_start=args.earnings_release_history_start,
        earnings_release_history_end=args.earnings_release_history_end,
        earnings_release_ciks=tuple(args.earnings_release_cik) or None,
        earnings_release_request_timeout=args.earnings_release_request_timeout,
        earnings_release_max_index_bytes=args.earnings_release_max_index_bytes,
        earnings_release_max_document_bytes=args.earnings_release_max_document_bytes,
        earnings_release_candidate_batch_size=args.earnings_release_candidate_batch_size,
        companyfacts_symbol_source=args.companyfacts_symbol_source,
        skip_loaded_companyfacts=args.skip_loaded_companyfacts,
        dry_run=args.dry_run,
        force=args.force,
        run_id=run_id,
    )


def _prune_activation_backups(db_path: Path, *, keep_latest: int) -> None:
    """Prune old pre-migrate backups after a successful governed migration run.

    Opens a READ-ONLY connection: pruning only deletes registered ``.bak`` files
    on disk and reads ``migration_backup_registry`` to know which are safe to
    remove (``enforce_backup_retention`` never writes to the database itself).
    Best-effort in the sense that a database file gone by the time this runs (or
    one that predates the backup registry table) simply prunes nothing.
    """
    import duckdb

    from .migration_admin import enforce_backup_retention

    con = duckdb.connect(str(db_path), read_only=True)
    try:
        enforce_backup_retention(
            db_path.parent,
            conn=con,
            keep_latest=keep_latest,
            min_age=dt.timedelta(0),
        )
    finally:
        con.close()


def run_activation_from_args(
    args: argparse.Namespace,
    *,
    governed_migrations: GovernedMigrationsCallable,
    run_activation: RunActivationCallable = run_activation,
) -> int:
    """Build options/stages from parsed ``activate`` args and run the ladder.

    On an existing warehouse file that has at least one PENDING migration, this
    runs ``governed_migrations`` -- checkpoint + backup + locked apply + verify
    with restore-on-failure -- BEFORE the ladder opens its own connection, so a
    bad migration on a live warehouse can never leave it half-upgraded. A
    warehouse already at head skips the governed path entirely: it copies the
    whole file (30-40 GB at target size) and re-hashes it, so paying that cost on
    every resume/``--only`` invocation -- the common case once a build is mostly
    done -- was the bug this guards against. A brand-new file has nothing to
    protect yet, so ``stage_migrate`` bootstraps it directly. ``--dry-run``
    never mutates anything, governed path included, so it is skipped there too.
    After a governed run actually executes, old backups beyond ``--backup-keep``
    are pruned. Used by the ``atx-db activate`` subcommand and by
    ``scripts/warehouse_activate.py``, which threads its own
    ``governed_migrations``/``run_activation`` injection seams through to this
    single implementation.
    """
    from .migration_admin import pending_migrations

    options = activation_options_from_args(args)
    stages = select_stages(
        start=args.start_stage,
        stop=args.stop_stage,
        only=tuple(args.only or ()),
    )
    _validate_submissions_resume_plan(options, stages)
    _validate_companyfacts_resume_plan(options, stages)
    if not options.dry_run and options.db_path.exists() and pending_migrations(options.db_path):
        governed_migrations(options.db_path)
        _prune_activation_backups(options.db_path, keep_latest=getattr(args, "backup_keep", 3))
    run_activation(options, stages=stages)
    return 0
