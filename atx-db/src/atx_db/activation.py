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
    "universe_us_listed",
    "provider_coverage",
)


@dataclass(frozen=True)
class StageResult:
    """What a stage did: a row count plus a JSON-serialisable detail payload."""

    rows: int
    detail: dict[str, object]


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
    ticker_history_expected_bytes: int | None = TICKER_HISTORY_UNCOMPRESSED_BYTES
    staging_dir: Path = Path("data/staging/broad-bars")
    cache_dir: Path = Path("data/cache")
    sec_user_agent: str | None = None
    downloader: Downloader | None = None
    memory_limit: str = "8GB"
    threads: int = 4
    reconciliation_shards: int = 16
    shard_runner: ShardRunner | None = None
    minimum_rows: int = 30_000_000
    minimum_securities: int = 10_000
    minimum_latest_date_securities: int = 5_000
    companyfacts_limit: int | None = None
    companyfacts_progress_every: int = 25
    skip_loaded_companyfacts: bool = True
    dry_run: bool = False
    force: bool = False
    run_id: str = "warehouse-activate"

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
            tsv_path=options.tsv_path.resolve(),
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
            run_id=f"{options.run_id}-submissions",
        ),
    )
    return StageResult(rows=int(result.rows_loaded), detail=dict(result.details))


def stage_companyfacts_load(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Load XBRL company facts for every CIK in sec_company_tickers (offline)."""
    from .fundamentals import SecCompanyFactsDataset, SecCompanyFactsOptions, resolve_companyfacts_targets

    if not options.companyfacts_zip.is_file():
        raise FileNotFoundError(options.companyfacts_zip)
    fact_options = SecCompanyFactsOptions(
        symbols=(),
        symbol_source="sec_company_tickers",
        symbol_limit=options.companyfacts_limit,
        skip_loaded_targets=options.skip_loaded_companyfacts,
        skip_failed_targets=True,
        companyfacts_zip=options.companyfacts_zip,
        as_of_date=options.as_of_date,
        refresh_derived_surfaces=False,
        progress_every_targets=options.companyfacts_progress_every,
        run_id=f"{options.run_id}-companyfacts",
    )
    # SecCompanyFactsDataset.load() raises when its target set resolves to empty,
    # treating "nothing to fetch" as an operator error (e.g. an unmapped symbol
    # list). With skip_loaded_targets on, a fully-loaded universe legitimately
    # resolves to zero targets on a repeat run -- that is success, not failure,
    # so the stage short-circuits to a no-op before it can raise. The no-op
    # detail carries the SAME key set as the normal-load branch below (zeros in
    # place of the row/target counts) so callers never have to branch on which
    # path ran.
    if not resolve_companyfacts_targets(store, fact_options):
        detail: dict[str, object] = {
            "symbols": fact_options.symbols,
            "symbol_source": "sec_company_tickers",
            "symbol_limit": fact_options.symbol_limit,
            "symbol_offset": fact_options.symbol_offset,
            "skip_loaded_targets": fact_options.skip_loaded_targets,
            "refresh_derived_surfaces": fact_options.refresh_derived_surfaces,
            "universe_id": fact_options.universe_id,
            "as_of_date": fact_options.as_of_date.isoformat() if fact_options.as_of_date else None,
            "target_count": 0,
            "loaded_targets": 0,
            "failed_target_count": 0,
            "source_mode": "bulk_zip",
            "companyfacts_zip": str(fact_options.companyfacts_zip) if fact_options.companyfacts_zip else None,
            "facts": 0,
            "fundamental_points": 0,
            "xbrl_concept_catalog": 0,
            "fundamental_fact_revisions": 0,
            "fundamental_statement_points": 0,
            "fundamental_periods": 0,
            "fundamental_ttm_points": 0,
            "unresolved_cik_candidate_rows": 0,
            "skip_loaded": options.skip_loaded_companyfacts,
        }
        return StageResult(rows=0, detail=detail)
    result = SecCompanyFactsDataset().run(store, fact_options)
    detail = {key: value for key, value in result.details.items() if key != "failed_targets"}
    detail["symbol_source"] = "sec_company_tickers"
    detail["skip_loaded"] = options.skip_loaded_companyfacts
    return StageResult(rows=int(result.rows_loaded), detail=detail)


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
        "companyfacts_load": stage_companyfacts_load,
    }
)


def stage_statement_points(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Rebuild the concept catalog, fact revisions, and normalized statement points."""
    from .fundamental_statements import refresh_fundamental_statement_points
    from .fundamentals import refresh_fundamental_fact_revisions, refresh_xbrl_concept_catalog

    _ = options
    catalog_rows = refresh_xbrl_concept_catalog(store)
    revision_rows = refresh_fundamental_fact_revisions(store)
    point_rows = refresh_fundamental_statement_points(store)
    return StageResult(
        rows=int(point_rows),
        detail={
            "catalog_rows": int(catalog_rows),
            "revision_rows": int(revision_rows),
            "statement_point_rows": int(point_rows),
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
    metric_count_row = store.con.execute("SELECT count(DISTINCT metric_code) FROM derived_metric_values").fetchone()
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


def stage_universe_us_listed(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Rebuild the point-in-time US-listed universe from bars + directory + deciles."""

    from .universe_us_listed import UniverseUsListedOptions, refresh_universe_us_listed

    rows = refresh_universe_us_listed(
        store,
        UniverseUsListedOptions(as_of_date=options.as_of_date, run_id=options.run_id),
    )
    return StageResult(rows=rows, detail={"table": "universe_us_listed_membership"})


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
        "universe_us_listed": stage_universe_us_listed,
        "provider_coverage": stage_provider_coverage,
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
                finish_stage(
                    store,
                    stage=stage,
                    run_id=options.run_id,
                    started_at=started_at,
                    status="failed",
                    rows=0,
                    error=str(exc),
                )
                payload = {
                    "stage": stage,
                    "status": "failed",
                    "rows": 0,
                    "seconds": round(time.monotonic() - begun, 3),
                    "detail": {"error": str(exc)},
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
    parser.add_argument("--staging-dir", type=Path, default=Path("data/staging/broad-bars"))
    parser.add_argument("--cache-dir", type=Path, default=Path("data/cache"))
    parser.add_argument("--sec-user-agent", default=None)
    parser.add_argument("--memory-limit", default="8GB")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--shards", type=int, default=16)
    parser.add_argument("--companyfacts-limit", type=int, default=None)
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
        staging_dir=args.staging_dir,
        cache_dir=args.cache_dir,
        sec_user_agent=sec_user_agent,
        downloader=requests_downloader,
        memory_limit=args.memory_limit,
        threads=args.threads,
        reconciliation_shards=args.shards,
        companyfacts_limit=args.companyfacts_limit,
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
    if not options.dry_run and options.db_path.exists() and pending_migrations(options.db_path):
        governed_migrations(options.db_path)
        _prune_activation_backups(options.db_path, keep_latest=getattr(args, "backup_keep", 3))
    run_activation(options, stages=stages)
    return 0
