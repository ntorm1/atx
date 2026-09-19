"""Idempotent, resumable, deterministic full-warehouse activation.

The ladder runs one stage at a time in dependency order, records every attempt in
``activation_stage_runs``, and skips a stage whose newest attempt completed unless
``force`` is set. Every stage is a pure ``(store, options) -> StageResult``; the
ladder owns all ledgering, ordering, and reporting so a stage function stays
testable on its own.
"""

from __future__ import annotations

import datetime as dt
import json
import logging
import os
from collections.abc import Callable
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Protocol

import pandas as pd

from .clock import resolve_as_of_date
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
        return dict(asdict(self)) | {"downloader": self.downloader}

    def ledger_params(self) -> dict[str, object]:
        """JSON-safe option snapshot for ``activation_stage_runs.params_json``."""
        payload = {key: value for key, value in self.as_dict().items() if key != "downloader"}
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
    record_source_file(
        store,
        dataset_id="sec_security_master",
        source_url=SEC_COMPANY_TICKERS_URL,
        cache_path=cache_path,
        status="available",
        metadata={"as_of_date": as_of_date.isoformat(), "bytes": byte_count},
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
    for url, normalizer, name in (
        (NASDAQ_LISTED_URL, normalize_nasdaq_listed, "nasdaqlisted.txt"),
        (OTHER_LISTED_URL, normalize_other_listed, "otherlisted.txt"),
    ):
        dest = Path(options.cache_dir) / name
        download(url, dest, user_agent=user_agent)
        texts.append((url, dest.read_text(encoding="utf-8"), normalizer))
    as_of_date = resolve_directory_as_of_date(directory_options, texts[0][1])
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
    # so the stage short-circuits to a no-op before it can raise.
    if not resolve_companyfacts_targets(store, fact_options):
        return StageResult(
            rows=0,
            detail={
                "symbol_source": "sec_company_tickers",
                "skip_loaded": options.skip_loaded_companyfacts,
                "target_count": 0,
                "loaded_targets": 0,
            },
        )
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
