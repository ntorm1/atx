"""Execute reconciliation-driven filing-context work with durable attempt state."""

from __future__ import annotations

import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from requests import exceptions as requests_exceptions

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .security_master import SEC_USER_AGENT
from .warehouse import quality_check
from .xbrl_filing_contexts import XbrlFilingContextDataset, XbrlFilingContextOptions

SOURCE_NAME = "filing_context_backfill_queue_executor_v1"
RETRYABLE_HTTP_STATUS_CODES = frozenset({408, 425, 429, 500, 502, 503, 504})


@dataclass(frozen=True)
class FilingContextBackfillExecutionOptions:
    max_filings: int = 10
    max_attempts_per_accession: int = 3
    stale_running_after_minutes: int = 30
    retry_failed: bool = True
    retry_nonretryable: bool = False
    stop_on_error: bool = False
    request_timeout: int = 120
    include_legacy_xbrl: bool = True
    use_source_cache: bool = True
    source_cache_dir: Path | None = None
    capture_filing_packages: bool = False
    user_agent: str = SEC_USER_AGENT
    run_id: str | None = None


@dataclass(frozen=True)
class FilingContextBackfillExecutionResult:
    attempted_count: int
    succeeded_count: int
    failed_count: int
    recovered_stale_count: int
    contexts_loaded: int
    dimensions_loaded: int
    facts_loaded: int
    estimated_request_count: int
    actual_request_count: int
    source_artifact_count: int
    source_cache_hit_count: int
    run_id: str


def _recover_stale_attempts(
    store: DuckDBStore,
    *,
    stale_running_after_minutes: int,
) -> int:
    row = store.con.execute(
        """
        SELECT count(*)
        FROM filing_context_backfill_attempts
        WHERE status='running'
          AND started_at < now() - (? * INTERVAL '1 minute')
        """,
        [stale_running_after_minutes],
    ).fetchone()
    if row is None:
        raise RuntimeError("stale filing-context attempt count returned no row")
    stale_count = int(row[0])
    if stale_count:
        store.con.execute(
            """
            UPDATE filing_context_backfill_attempts
            SET status='failed',
                is_retryable=attempt_number < max_attempts,
                completed_at=now(),
                error_type='stale_worker_lease',
                error_message='Recovered a running attempt whose worker lease expired',
                source_loaded_at=now()
            WHERE status='running'
              AND started_at < now() - (? * INTERVAL '1 minute')
            """,
            [stale_running_after_minutes],
        )
    return stale_count


def _candidate_rows(
    store: DuckDBStore,
    *,
    max_filings: int,
    max_attempts_per_accession: int,
    retry_failed: bool,
    retry_nonretryable: bool,
) -> list[dict[str, Any]]:
    frame = store.con.execute(
        """
        WITH attempt_summary AS (
            SELECT
                queue_id,
                count(*) AS attempt_count,
                count(*) FILTER (WHERE status='running') AS running_count,
                arg_max(status,attempt_number) AS latest_status,
                arg_max(is_retryable,attempt_number) AS latest_is_retryable
            FROM filing_context_backfill_attempts
            GROUP BY queue_id
        )
        SELECT
            queue.queue_id,
            queue.build_id AS queue_build_id,
            queue.security_id,
            queue.symbol,
            queue.cik,
            queue.accession_number,
            queue.priority_tier,
            queue.priority_rank,
            queue.estimated_request_count,
            queue.available_at AS queue_available_at,
            coalesce(attempts.attempt_count,0)+1 AS attempt_number
        FROM filing_context_backfill_queue queue
        LEFT JOIN attempt_summary attempts USING (queue_id)
        WHERE queue.queue_status='pending'
          AND NOT queue.has_existing_filing_context
          AND coalesce(attempts.running_count,0)=0
          AND coalesce(attempts.attempt_count,0) < ?
          AND (
              attempts.queue_id IS NULL
              OR (
                  ?
                  AND attempts.latest_status='failed'
                  AND (attempts.latest_is_retryable OR ?)
              )
          )
        ORDER BY queue.priority_rank,queue.security_id,queue.accession_number
        LIMIT ?
        """,
        [max_attempts_per_accession, retry_failed, retry_nonretryable, max_filings],
    ).df()
    return [
        {str(key): value for key, value in row.items()}
        for row in frame.to_dict("records")
    ]


def _claim_attempt(
    store: DuckDBStore,
    *,
    candidate: dict[str, Any],
    attempt_id: str,
    max_attempts: int,
    run_id: str,
) -> bool:
    with store.transaction():
        running_row = store.con.execute(
            """
            SELECT count(*)
            FROM filing_context_backfill_attempts
            WHERE queue_id=? AND status='running'
            """,
            [candidate["queue_id"]],
        ).fetchone()
        if running_row is None:
            raise RuntimeError("filing-context running-attempt claim query returned no row")
        if int(running_row[0]):
            return False
        store.con.execute(
            """
            UPDATE filing_context_backfill_attempts
            SET is_latest_revision=false
            WHERE queue_id=? AND is_latest_revision
            """,
            [candidate["queue_id"]],
        )
        store.con.execute(
            """
            INSERT INTO filing_context_backfill_attempts (
                attempt_id,queue_id,queue_build_id,security_id,symbol,cik,
                accession_number,priority_tier,priority_rank,attempt_number,
                max_attempts,status,is_retryable,estimated_request_count,
                started_at,as_of_date,available_at,is_latest_revision,run_id,
                source_loaded_at
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,'running',false,?,now(),
                      CAST(now() AS DATE),now(),true,?,now())
            """,
            [
                attempt_id,
                candidate["queue_id"],
                candidate["queue_build_id"],
                candidate["security_id"],
                candidate["symbol"],
                candidate["cik"],
                candidate["accession_number"],
                candidate["priority_tier"],
                int(candidate["priority_rank"]),
                int(candidate["attempt_number"]),
                max_attempts,
                int(candidate["estimated_request_count"]),
                run_id,
            ],
        )
    return True


def _finish_success(
    store: DuckDBStore,
    *,
    attempt_id: str,
    contexts: int,
    dimensions: int,
    facts: int,
    actual_request_count: int,
    source_artifact_count: int,
    source_cache_hit_count: int,
) -> None:
    store.con.execute(
        """
        UPDATE filing_context_backfill_attempts
        SET status='succeeded',is_retryable=false,completed_at=now(),
            contexts_loaded=?,dimensions_loaded=?,facts_loaded=?,
            actual_request_count=?,source_artifact_count=?,source_cache_hit_count=?,
            source_loaded_at=now()
        WHERE attempt_id=?
        """,
        [
            contexts,
            dimensions,
            facts,
            actual_request_count,
            source_artifact_count,
            source_cache_hit_count,
            attempt_id,
        ],
    )


def _finish_failure(
    store: DuckDBStore,
    *,
    attempt_id: str,
    attempt_number: int,
    max_attempts: int,
    exc: Exception,
) -> None:
    is_retryable = attempt_number < max_attempts and _is_retryable_exception(exc)
    store.con.execute(
        """
        UPDATE filing_context_backfill_attempts
        SET status='failed',is_retryable=?,completed_at=now(),
            error_type=?,error_message=?,source_loaded_at=now()
        WHERE attempt_id=?
        """,
        [
            is_retryable,
            type(exc).__name__,
            str(exc)[:4000],
            attempt_id,
        ],
    )


def _is_retryable_exception(exc: Exception) -> bool:
    """Return whether another identical attempt can plausibly succeed.

    Transport interruptions and the same HTTP statuses configured on the shared
    SEC client are transient. Parser, validation, URL, and schema errors require
    intervention and are kept out of automatic retry loops.
    """

    if isinstance(exc, requests_exceptions.HTTPError):
        response = exc.response
        return response is not None and response.status_code in RETRYABLE_HTTP_STATUS_CODES
    return isinstance(
        exc,
        (
            requests_exceptions.ConnectionError,
            requests_exceptions.Timeout,
            requests_exceptions.ChunkedEncodingError,
            requests_exceptions.ContentDecodingError,
            requests_exceptions.RetryError,
        ),
    )


def execute_filing_context_backfill(
    store: DuckDBStore,
    options: FilingContextBackfillExecutionOptions | None = None,
) -> FilingContextBackfillExecutionResult:
    """Claim and execute a bounded, priority-ordered batch of filing instances."""

    options = options or FilingContextBackfillExecutionOptions()
    if options.max_filings < 1:
        raise ValueError("max_filings must be positive")
    if options.max_attempts_per_accession < 1:
        raise ValueError("max_attempts_per_accession must be positive")
    if options.stale_running_after_minutes < 1:
        raise ValueError("stale_running_after_minutes must be positive")
    store.initialize()
    run_id = options.run_id or f"filing-context-backfill-execution-{uuid.uuid4()}"
    recovered_stale_count = _recover_stale_attempts(
        store,
        stale_running_after_minutes=options.stale_running_after_minutes,
    )
    candidates = _candidate_rows(
        store,
        max_filings=options.max_filings,
        max_attempts_per_accession=options.max_attempts_per_accession,
        retry_failed=options.retry_failed,
        retry_nonretryable=options.retry_nonretryable,
    )

    attempted_count = 0
    succeeded_count = 0
    failed_count = 0
    contexts_loaded = 0
    dimensions_loaded = 0
    facts_loaded = 0
    estimated_request_count = 0
    actual_request_count = 0
    source_artifact_count = 0
    source_cache_hit_count = 0
    loader = XbrlFilingContextDataset()
    for candidate in candidates:
        attempt_id = str(uuid.uuid4())
        if not _claim_attempt(
            store,
            candidate=candidate,
            attempt_id=attempt_id,
            max_attempts=options.max_attempts_per_accession,
            run_id=run_id,
        ):
            continue
        attempted_count += 1
        estimated_request_count += int(candidate["estimated_request_count"])
        attempt_number = int(candidate["attempt_number"])
        try:
            load_result = loader.load(
                store,
                XbrlFilingContextOptions(
                    symbols=(),
                    forms=(),
                    accession_numbers=(str(candidate["accession_number"]),),
                    max_filings=1,
                    request_timeout=options.request_timeout,
                    include_legacy_xbrl=options.include_legacy_xbrl,
                    use_source_cache=options.use_source_cache,
                    source_cache_dir=options.source_cache_dir,
                    capture_filing_package=options.capture_filing_packages,
                    user_agent=options.user_agent,
                    run_id=run_id,
                ),
            )
            details = load_result.details
            loaded_context_count = int(details.get("contexts", 0))
            loaded_dimension_count = int(details.get("dimensions", 0))
            loaded_fact_count = int(details.get("facts", 0))
            loaded_request_count = int(
                details.get("requests", candidate["estimated_request_count"])
            )
            loaded_source_artifact_count = int(
                details.get("source_artifacts", loaded_request_count)
            )
            loaded_source_cache_hit_count = int(details.get("source_cache_hits", 0))
            _finish_success(
                store,
                attempt_id=attempt_id,
                contexts=loaded_context_count,
                dimensions=loaded_dimension_count,
                facts=loaded_fact_count,
                actual_request_count=loaded_request_count,
                source_artifact_count=loaded_source_artifact_count,
                source_cache_hit_count=loaded_source_cache_hit_count,
            )
            succeeded_count += 1
            contexts_loaded += loaded_context_count
            dimensions_loaded += loaded_dimension_count
            facts_loaded += loaded_fact_count
            actual_request_count += loaded_request_count
            source_artifact_count += loaded_source_artifact_count
            source_cache_hit_count += loaded_source_cache_hit_count
        except Exception as exc:
            _finish_failure(
                store,
                attempt_id=attempt_id,
                attempt_number=attempt_number,
                max_attempts=options.max_attempts_per_accession,
                exc=exc,
            )
            failed_count += 1
            if options.stop_on_error:
                raise

    return FilingContextBackfillExecutionResult(
        attempted_count=attempted_count,
        succeeded_count=succeeded_count,
        failed_count=failed_count,
        recovered_stale_count=recovered_stale_count,
        contexts_loaded=contexts_loaded,
        dimensions_loaded=dimensions_loaded,
        facts_loaded=facts_loaded,
        estimated_request_count=estimated_request_count,
        actual_request_count=actual_request_count,
        source_artifact_count=source_artifact_count,
        source_cache_hit_count=source_cache_hit_count,
        run_id=run_id,
    )


class FilingContextBackfillExecutorDataset(Dataset):
    dataset_id = "filing_context_backfill_attempts"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self,
        store: DuckDBStore,
        options: FilingContextBackfillExecutionOptions,
    ) -> DatasetLoadResult:
        result = execute_filing_context_backfill(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="filing_context_backfill_attempts",
            check_name="batch_attempt_failures",
            status="passed" if result.failed_count == 0 else "warning",
            observed_value=float(result.failed_count),
            threshold_value=0.0,
            details={
                "attempted": result.attempted_count,
                "succeeded": result.succeeded_count,
                "recovered_stale": result.recovered_stale_count,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=result.contexts_loaded + result.facts_loaded,
            source=self.source_name,
            run_id=result.run_id,
            details={
                "attempted": result.attempted_count,
                "succeeded": result.succeeded_count,
                "failed": result.failed_count,
                "contexts": result.contexts_loaded,
                "dimensions": result.dimensions_loaded,
                "facts": result.facts_loaded,
                "estimated_requests": result.estimated_request_count,
                "actual_requests": result.actual_request_count,
                "source_artifacts": result.source_artifact_count,
                "source_cache_hits": result.source_cache_hit_count,
            },
        )
