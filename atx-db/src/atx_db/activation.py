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
import subprocess
import sys
import time
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
        store, CalendarizationOptions(run_id=f"{options.run_id}-calendarization")
    )
    rows = int(summary.get("calendar_ttm_rows", summary.get("map_rows", 0)) or 0)
    return StageResult(rows=rows, detail=dict(summary))


def stage_standardized(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .standardization import FundamentalStandardizationOptions, refresh_fundamental_standardized

    _configure_analytical_session(store, options)
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


def stage_reconciliation(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Publish the reconciliation serving table in bounded symbol shards.

    Side effect: this stage CLOSES the ladder's connection and shells out to
    ``scripts/refresh_reconciliation_sharded.py``, which runs each shard in a fresh
    interpreter. A long-lived process was measured to degrade a shard from ~110s to
    ~840s at identical warehouse size, and DuckDB allows exactly one writer, so the
    ladder must yield the file for the duration. The connection is reopened before
    returning, so the stage's contract to its caller is unchanged.
    """
    script = Path(__file__).resolve().parents[2] / "scripts" / "refresh_reconciliation_sharded.py"
    db_path = str(options.db_path)
    store.close()
    try:
        completed = subprocess.run(
            [
                sys.executable,
                str(script),
                "--db-path",
                db_path,
                "--shards",
                str(options.reconciliation_shards),
                "--memory-limit",
                options.memory_limit,
                "--threads",
                str(options.threads),
                "--run-id-prefix",
                f"{options.run_id}-recon",
            ],
            check=False,
        )
    finally:
        store.reopen()
    if completed.returncode != 0:
        raise RuntimeError(
            f"sharded reconciliation publish failed with exit code {completed.returncode}"
        )
    row = store.con.execute("SELECT count(*) FROM fundamental_reconciliation_serving").fetchone()
    rows = 0 if row is None else int(row[0])
    return StageResult(rows=rows, detail={"shards": options.reconciliation_shards, "serving_rows": rows})


def stage_provider_coverage(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .provider_coverage import ProviderCoverageOptions, refresh_provider_coverage

    rows = refresh_provider_coverage(
        store,
        ProviderCoverageOptions(run_id=f"{options.run_id}-coverage"),
    )
    conditions = {
        condition: sum(row.condition == condition for row in rows)
        for condition in ("available", "degraded", "pending", "missing")
    }
    return StageResult(rows=len(rows), detail={"schema_count": len(rows), "conditions": conditions})


def _configure_analytical_session(store: DuckDBStore, options: ActivationOptions) -> None:
    store.con.execute("PRAGMA disable_progress_bar")
    store.con.execute("SET memory_limit = ?", [options.memory_limit])
    store.con.execute("SET threads = ?", [options.threads])
    store.con.execute("SET preserve_insertion_order = false")


STAGES.update(
    {
        "statement_points": stage_statement_points,
        "periods": stage_periods,
        "ttm": stage_ttm,
        "calendarization": stage_calendarization,
        "standardized": stage_standardized,
        "industry_templates": stage_industry_templates,
        "reconciliation": stage_reconciliation,
        "provider_coverage": stage_provider_coverage,
    }
)


def print_json(payload: dict[str, object]) -> None:
    """Emit one JSON line to stdout, flushed, so an operator can tail the ladder."""
    print(json.dumps(payload, default=str, sort_keys=True), flush=True)


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
    params = options.ledger_params()
    with DuckDBStore(options.db_path) as store:
        done = set() if options.force else completed_stages(store)
        for stage in stages:
            if options.dry_run:
                payload = {
                    "stage": stage,
                    "status": "dry_run",
                    "rows": 0,
                    "seconds": 0.0,
                    "detail": {"already_completed": stage in done},
                }
                emit(payload)
                emitted.append(payload)
                continue
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
