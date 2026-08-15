from __future__ import annotations

from requests.exceptions import ConnectionError as RequestsConnectionError

from atx_db.dataset import DatasetLoadResult
from atx_db.filing_context_backfill_executor import (
    FilingContextBackfillExecutionOptions,
    execute_filing_context_backfill,
)
from atx_db.quality import run_warehouse_quality_checks
from atx_db.xbrl_filing_contexts import XbrlFilingContextDataset


def _insert_queue_row(store, *, queue_id: str, accession: str, priority_rank: int) -> None:
    store.con.execute(
        """
        INSERT INTO filing_context_backfill_queue (
            queue_id,build_id,security_id,symbol,cik,accession_number,
            has_existing_filing_context,filing_directory_url,filing_index_url,
            estimated_request_count,affected_reconciliation_count,
            affected_error_rule_count,affected_rule_count,affected_period_count,
            mismatch_count,diagnostic_difference_count,unverified_reconciled_count,
            priority_tier,priority_score,priority_rank,queue_status,
            source_max_available_at,as_of_date,available_at,is_latest_revision,
            run_id,source_loaded_at
        ) VALUES (
            ?,'queue-build','SEC-CIK-0000320193','AAPL','0000320193',?,
            false,'https://example.test/directory','https://example.test/index.json',
            1,1,1,1,1,1,0,0,'P0',1000000,?,'pending',
            TIMESTAMP '2026-08-14 10:00:00',DATE '2026-08-14',
            TIMESTAMP '2026-08-14 10:00:00',true,'queue-run',
            TIMESTAMP '2026-08-14 10:01:00'
        )
        """,
        [queue_id, accession, priority_rank],
    )


def _successful_load(_self, _store, options) -> DatasetLoadResult:
    return DatasetLoadResult(
        dataset_id="xbrl_filing_contexts",
        rows_loaded=14,
        source="fixture",
        run_id=options.run_id,
        details={"contexts": 4, "dimensions": 6, "facts": 10},
    )


def test_executor_claims_priority_order_and_does_not_repeat_success(
    tmp_store,
    monkeypatch,
) -> None:
    _insert_queue_row(
        tmp_store,
        queue_id="queue-lower-priority",
        accession="0000320193-26-000002",
        priority_rank=2,
    )
    _insert_queue_row(
        tmp_store,
        queue_id="queue-higher-priority",
        accession="0000320193-26-000001",
        priority_rank=1,
    )
    monkeypatch.setattr(XbrlFilingContextDataset, "load", _successful_load)

    first = execute_filing_context_backfill(
        tmp_store,
        FilingContextBackfillExecutionOptions(max_filings=1, run_id="executor-first"),
    )
    second = execute_filing_context_backfill(
        tmp_store,
        FilingContextBackfillExecutionOptions(max_filings=10, run_id="executor-second"),
    )
    third = execute_filing_context_backfill(
        tmp_store,
        FilingContextBackfillExecutionOptions(max_filings=10, run_id="executor-third"),
    )

    assert (first.attempted_count, first.succeeded_count, first.contexts_loaded) == (1, 1, 4)
    assert (second.attempted_count, second.succeeded_count) == (1, 1)
    assert third.attempted_count == 0
    rows = tmp_store.con.execute(
        """
        SELECT queue_id,status,contexts_loaded,dimensions_loaded,facts_loaded,run_id
        FROM filing_context_backfill_attempts
        ORDER BY started_at,priority_rank
        """
    ).fetchall()
    assert rows == [
        ("queue-higher-priority", "succeeded", 4, 6, 10, "executor-first"),
        ("queue-lower-priority", "succeeded", 4, 6, 10, "executor-second"),
    ]


def test_executor_retries_transport_failures_to_budget_and_marks_exhaustion(
    tmp_store, monkeypatch
) -> None:
    _insert_queue_row(
        tmp_store,
        queue_id="queue-failure",
        accession="0000320193-26-000003",
        priority_rank=1,
    )

    def failed_load(_self, _store, _options):
        raise RequestsConnectionError("fixture transport failure")

    monkeypatch.setattr(XbrlFilingContextDataset, "load", failed_load)
    options = FilingContextBackfillExecutionOptions(
        max_filings=1,
        max_attempts_per_accession=2,
    )

    first = execute_filing_context_backfill(tmp_store, options)
    second = execute_filing_context_backfill(tmp_store, options)
    third = execute_filing_context_backfill(tmp_store, options)

    assert (first.failed_count, second.failed_count, third.attempted_count) == (1, 1, 0)
    attempts = tmp_store.con.execute(
        """
        SELECT attempt_number,status,is_retryable,is_latest_revision,error_type
        FROM filing_context_backfill_attempts
        ORDER BY attempt_number
        """
    ).fetchall()
    assert attempts == [
        (1, "failed", True, False, "ConnectionError"),
        (2, "failed", False, True, "ConnectionError"),
    ]
    checks = run_warehouse_quality_checks(
        tmp_store,
        dataset_ids=("filing_context_backfill_attempts",),
        record=False,
    )
    by_name = {check.check_name: check for check in checks}
    assert by_name["bad_filing_context_backfill_attempt_rows"].status == "passed"
    assert by_name["duplicate_filing_context_backfill_attempt_sequences"].status == "passed"
    assert by_name["stale_running_filing_context_backfill_attempts"].status == "passed"
    assert by_name["exhausted_filing_context_backfill_attempts"].status == "warning"


def test_executor_does_not_automatically_retry_deterministic_parser_failure(
    tmp_store, monkeypatch
) -> None:
    _insert_queue_row(
        tmp_store,
        queue_id="queue-parser-failure",
        accession="0000320193-26-000009",
        priority_rank=1,
    )

    def failed_load(_self, _store, _options):
        raise ValueError("fixture parse failure")

    monkeypatch.setattr(XbrlFilingContextDataset, "load", failed_load)
    options = FilingContextBackfillExecutionOptions(
        max_filings=1,
        max_attempts_per_accession=3,
    )

    first = execute_filing_context_backfill(tmp_store, options)
    skipped = execute_filing_context_backfill(tmp_store, options)
    forced = execute_filing_context_backfill(
        tmp_store,
        FilingContextBackfillExecutionOptions(
            max_filings=1,
            max_attempts_per_accession=3,
            retry_nonretryable=True,
        ),
    )

    assert (first.failed_count, skipped.attempted_count, forced.failed_count) == (1, 0, 1)
    assert tmp_store.con.execute(
        """
        SELECT attempt_number,status,is_retryable,is_latest_revision,error_type
        FROM filing_context_backfill_attempts
        ORDER BY attempt_number
        """
    ).fetchall() == [
        (1, "failed", False, False, "ValueError"),
        (2, "failed", False, True, "ValueError"),
    ]


def test_executor_recovers_stale_running_attempt_before_retry(tmp_store, monkeypatch) -> None:
    _insert_queue_row(
        tmp_store,
        queue_id="queue-stale",
        accession="0000320193-26-000004",
        priority_rank=1,
    )
    tmp_store.con.execute(
        """
        INSERT INTO filing_context_backfill_attempts (
            attempt_id,queue_id,queue_build_id,security_id,symbol,cik,
            accession_number,priority_tier,priority_rank,attempt_number,
            max_attempts,status,is_retryable,estimated_request_count,
            started_at,as_of_date,available_at,is_latest_revision,run_id,
            source_loaded_at
        ) VALUES (
            'stale-attempt','queue-stale','queue-build','SEC-CIK-0000320193',
            'AAPL','0000320193','0000320193-26-000004','P0',1,1,3,
            'running',false,1,TIMESTAMP '2026-08-14 08:00:00',
            DATE '2026-08-14',TIMESTAMP '2026-08-14 08:00:00',true,
            'stale-run',TIMESTAMP '2026-08-14 08:00:00'
        )
        """
    )
    monkeypatch.setattr(XbrlFilingContextDataset, "load", _successful_load)

    result = execute_filing_context_backfill(
        tmp_store,
        FilingContextBackfillExecutionOptions(
            max_filings=1,
            stale_running_after_minutes=1,
            run_id="recovery-run",
        ),
    )

    assert result.recovered_stale_count == 1
    assert (result.attempted_count, result.succeeded_count) == (1, 1)
    attempts = tmp_store.con.execute(
        """
        SELECT attempt_number,status,is_latest_revision,error_type
        FROM filing_context_backfill_attempts
        ORDER BY attempt_number
        """
    ).fetchall()
    assert attempts == [
        (1, "failed", False, "stale_worker_lease"),
        (2, "succeeded", True, None),
    ]
