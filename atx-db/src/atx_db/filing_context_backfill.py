"""Prioritize SEC filing-instance loads from unresolved reconciliation evidence."""

from __future__ import annotations

import datetime as dt
import uuid
from dataclasses import dataclass
from typing import cast

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import quality_check

SOURCE_NAME = "fundamental_reconciliation_context_gaps_v1"


@dataclass(frozen=True)
class FilingContextBackfillQueueOptions:
    run_id: str | None = None


@dataclass(frozen=True)
class FilingContextBackfillQueueResult:
    build_id: str
    queue_row_count: int
    ready_row_count: int
    blocked_row_count: int
    affected_reconciliation_count: int
    max_available_at: dt.datetime | None
    run_id: str


def _latest_reconciliation_build(store: DuckDBStore) -> tuple[str | None, int | None]:
    row = store.con.execute(
        """
        SELECT build_id,published_content_hash
        FROM fundamental_reconciliation_builds
        WHERE status='completed'
        ORDER BY completed_at DESC,build_id DESC
        LIMIT 1
        """
    ).fetchone()
    if row is not None:
        return str(row[0]), None if row[1] is None else int(row[1])
    serving_row = store.con.execute(
        "SELECT count(*) FROM fundamental_reconciliation_serving"
    ).fetchone()
    if serving_row is None:
        raise RuntimeError("reconciliation serving count query returned no row")
    serving_count = int(serving_row[0])
    if serving_count:
        raise RuntimeError("reconciliation serving rows exist without a completed publication manifest")
    return None, None


def _published_queue_state(
    store: DuckDBStore,
) -> tuple[int, int, int, int, dt.datetime | None, int | None]:
    row = store.con.execute(
        """
        SELECT
            count(*),
            count(*) FILTER (WHERE queue_status='pending'),
            count(*) FILTER (WHERE queue_status<>'pending'),
            coalesce(sum(affected_reconciliation_count),0),
            max(available_at),
            bit_xor(hash(to_json(queue_row)))
        FROM filing_context_backfill_queue queue_row
        """
    ).fetchone()
    if row is None:
        raise RuntimeError("filing-context queue publication query returned no row")
    return (
        int(row[0]),
        int(row[1]),
        int(row[2]),
        int(row[3]),
        row[4],
        None if row[5] is None else int(row[5]),
    )


def _queue_input_watermark(store: DuckDBStore) -> dt.datetime | None:
    row = store.con.execute(
        """
        SELECT max(input_watermark)
        FROM (
            SELECT max(source_loaded_at) AS input_watermark
            FROM fundamental_reconciliation_serving
            UNION ALL
            SELECT max(source_loaded_at) FROM sec_submissions
            UNION ALL
            SELECT max(source_loaded_at) FROM xbrl_filing_contexts
        )
        """
    ).fetchone()
    if row is None:
        raise RuntimeError("filing-context queue input-watermark query returned no row")
    return cast(dt.datetime | None, row[0])


def refresh_filing_context_backfill_queue(
    store: DuckDBStore,
    options: FilingContextBackfillQueueOptions | None = None,
) -> FilingContextBackfillQueueResult:
    """Atomically publish one queue row per unresolved single-filing accession."""

    options = options or FilingContextBackfillQueueOptions()
    store.initialize()
    run_id = options.run_id or f"filing-context-backfill-queue-{uuid.uuid4()}"
    build_id = str(uuid.uuid4())
    input_build_id, input_content_hash = _latest_reconciliation_build(store)
    store.con.execute(
        """
        INSERT INTO filing_context_backfill_builds (
            build_id,status,input_reconciliation_build_id,
            input_reconciliation_content_hash,started_at,run_id
        ) VALUES (?,'running',?,?,now(),?)
        """,
        [build_id, input_build_id, input_content_hash, run_id],
    )
    try:
        store.con.execute(
            """
            CREATE OR REPLACE TEMP TABLE filing_context_backfill_queue_next AS
            WITH gap_rows AS (
                SELECT
                    reconciliation_id,
                    security_id,
                    symbol,
                    cik,
                    json_extract_string(input_accessions_json,'$[0]') AS accession_number,
                    rule_id,
                    period_end,
                    mismatch_severity,
                    status,
                    absolute_difference,
                    residual_percent,
                    available_at,
                    source_loaded_at
                FROM fundamental_reconciliation_serving
                WHERE is_latest_revision
                  AND is_applicable
                  AND input_filing_status='single_filing'
                  AND input_accession_count=1
                  AND context_verification_status='context_not_loaded'
                  AND input_accessions_json IS NOT NULL
                  AND available_at IS NOT NULL
            ), aggregated AS (
                SELECT
                    security_id,
                    any_value(symbol) AS symbol,
                    any_value(cik) AS cik,
                    accession_number,
                    count(DISTINCT reconciliation_id) AS affected_reconciliation_count,
                    count(DISTINCT reconciliation_id) FILTER (
                        WHERE mismatch_severity='error'
                    ) AS affected_error_rule_count,
                    count(DISTINCT rule_id) AS affected_rule_count,
                    count(DISTINCT period_end) AS affected_period_count,
                    count(*) FILTER (WHERE status='mismatch') AS mismatch_count,
                    count(*) FILTER (WHERE status='diagnostic_difference')
                        AS diagnostic_difference_count,
                    count(*) FILTER (WHERE status='reconciled')
                        AS unverified_reconciled_count,
                    max(absolute_difference) AS max_absolute_difference,
                    max(abs(residual_percent)) AS max_abs_residual_percent,
                    max(available_at) AS source_max_available_at,
                    max(source_loaded_at) AS reconciliation_source_loaded_at
                FROM gap_rows
                WHERE accession_number IS NOT NULL AND accession_number<>''
                GROUP BY security_id,accession_number
            ), submissions AS (
                SELECT * EXCLUDE (submission_rank)
                FROM (
                    SELECT
                        s.*,
                        row_number() OVER (
                            PARTITION BY s.security_id,s.accession_number
                            ORDER BY s.source_loaded_at DESC,s.acceptance_datetime DESC NULLS LAST
                        ) AS submission_rank
                    FROM sec_submissions s
                )
                WHERE submission_rank=1
            ), existing_contexts AS (
                SELECT security_id,accession_number,count(*) AS context_count
                FROM xbrl_filing_contexts
                GROUP BY security_id,accession_number
            ), enriched AS (
                SELECT
                    sha256(
                        'filing-context-backfill|' || gaps.security_id || '|' ||
                        gaps.accession_number
                    ) AS queue_id,
                    ?::VARCHAR AS build_id,
                    gaps.security_id,
                    gaps.symbol,
                    gaps.cik,
                    gaps.accession_number,
                    submission.form,
                    submission.filing_date,
                    submission.report_date,
                    submission.acceptance_datetime,
                    submission.primary_document,
                    submission.is_xbrl,
                    submission.is_inline_xbrl,
                    coalesce(contexts.context_count,0)>0 AS has_existing_filing_context,
                    CASE
                        WHEN submission.security_id IS NULL THEN NULL
                        WHEN NOT coalesce(submission.is_xbrl,false) THEN NULL
                        WHEN coalesce(submission.is_inline_xbrl,false) THEN 'inline_xbrl'
                        ELSE 'xbrl_xml'
                    END AS expected_instance_format,
                    'https://www.sec.gov/Archives/edgar/data/' || ltrim(gaps.cik,'0') ||
                        '/' || replace(gaps.accession_number,'-','') AS filing_directory_url,
                    'https://www.sec.gov/Archives/edgar/data/' || ltrim(gaps.cik,'0') ||
                        '/' || replace(gaps.accession_number,'-','') || '/index.json'
                        AS filing_index_url,
                    CASE
                        WHEN submission.primary_document IS NULL OR submission.primary_document=''
                            THEN NULL
                        ELSE 'https://www.sec.gov/Archives/edgar/data/' || ltrim(gaps.cik,'0') ||
                            '/' || replace(gaps.accession_number,'-','') || '/' ||
                            ltrim(submission.primary_document,'/')
                    END AS primary_document_url,
                    submission.size AS filing_size_bytes,
                    gaps.affected_reconciliation_count,
                    gaps.affected_error_rule_count,
                    gaps.affected_rule_count,
                    gaps.affected_period_count,
                    gaps.mismatch_count,
                    gaps.diagnostic_difference_count,
                    gaps.unverified_reconciled_count,
                    gaps.max_absolute_difference,
                    gaps.max_abs_residual_percent,
                    gaps.source_max_available_at,
                    greatest(
                        gaps.reconciliation_source_loaded_at,
                        submission.source_loaded_at
                    ) AS input_source_loaded_at,
                    CASE
                        WHEN submission.security_id IS NULL THEN 'blocked'
                        WHEN NOT coalesce(submission.is_xbrl,false) THEN 'blocked'
                        WHEN submission.primary_document IS NULL OR submission.primary_document=''
                            THEN 'blocked'
                        ELSE 'pending'
                    END AS queue_status,
                    CASE
                        WHEN submission.security_id IS NULL THEN 'missing_sec_submission'
                        WHEN NOT coalesce(submission.is_xbrl,false) THEN 'submission_not_xbrl'
                        WHEN submission.primary_document IS NULL OR submission.primary_document=''
                            THEN 'missing_primary_document'
                        ELSE NULL
                    END AS blocked_reason
                FROM aggregated gaps
                LEFT JOIN submissions submission
                  ON submission.security_id=gaps.security_id
                 AND submission.accession_number=gaps.accession_number
                LEFT JOIN existing_contexts contexts
                  ON contexts.security_id=gaps.security_id
                 AND contexts.accession_number=gaps.accession_number
            ), scored AS (
                SELECT
                    enriched.*,
                    CASE
                        WHEN mismatch_count>0 THEN 'P0'
                        WHEN diagnostic_difference_count>0 THEN 'P1'
                        WHEN affected_error_rule_count>0 THEN 'P2'
                        ELSE 'P3'
                    END AS priority_tier,
                    (
                        CASE
                            WHEN mismatch_count>0 THEN 1000000000
                            WHEN diagnostic_difference_count>0 THEN 100000000
                            WHEN affected_error_rule_count>0 THEN 10000000
                            ELSE 0
                        END
                        + mismatch_count*1000000
                        + diagnostic_difference_count*100000
                        + affected_error_rule_count*10000
                        + affected_reconciliation_count*100
                        + affected_period_count
                    )::BIGINT AS priority_score,
                    CASE
                        WHEN queue_status<>'pending' THEN 0
                        WHEN expected_instance_format='inline_xbrl' THEN 1
                        ELSE 2
                    END::INTEGER AS estimated_request_count
                FROM enriched
            ), ranked AS (
                SELECT
                    scored.*,
                    row_number() OVER (
                        ORDER BY
                            CASE WHEN queue_status='pending' THEN 0 ELSE 1 END,
                            priority_score DESC,
                            filing_date DESC NULLS LAST,
                            security_id,
                            accession_number
                    ) AS priority_rank
                FROM scored
            )
            SELECT
                queue_id,
                build_id,
                security_id,
                symbol,
                cik,
                accession_number,
                form,
                filing_date,
                report_date,
                acceptance_datetime,
                primary_document,
                is_xbrl,
                is_inline_xbrl,
                has_existing_filing_context,
                expected_instance_format,
                filing_directory_url,
                filing_index_url,
                primary_document_url,
                filing_size_bytes,
                estimated_request_count,
                affected_reconciliation_count,
                affected_error_rule_count,
                affected_rule_count,
                affected_period_count,
                mismatch_count,
                diagnostic_difference_count,
                unverified_reconciled_count,
                max_absolute_difference,
                max_abs_residual_percent,
                priority_tier,
                priority_score,
                priority_rank,
                queue_status,
                blocked_reason,
                source_max_available_at,
                CAST(source_max_available_at AS DATE) AS as_of_date,
                source_max_available_at AS available_at,
                true AS is_latest_revision,
                ?::VARCHAR AS run_id,
                now() AS source_loaded_at
            FROM ranked
            """,
            [build_id, run_id],
        )
        source_gap_row = store.con.execute(
            """
            SELECT coalesce(sum(affected_reconciliation_count),0)
            FROM filing_context_backfill_queue_next
            """
        ).fetchone()
        if source_gap_row is None:
            raise RuntimeError("filing-context source-gap count query returned no row")
        source_gap_row_count = int(source_gap_row[0])
        input_max_source_loaded_at = _queue_input_watermark(store)
        with store.transaction():
            store.con.execute(
                """
                INSERT OR REPLACE INTO filing_context_backfill_queue
                SELECT * FROM filing_context_backfill_queue_next
                """
            )
            store.con.execute(
                """
                DELETE FROM filing_context_backfill_queue current_row
                WHERE NOT EXISTS (
                    SELECT 1
                    FROM filing_context_backfill_queue_next next_row
                    WHERE next_row.queue_id=current_row.queue_id
                )
                """
            )
            (
                queue_row_count,
                ready_row_count,
                blocked_row_count,
                affected_reconciliation_count,
                max_available_at,
                published_content_hash,
            ) = _published_queue_state(store)
            store.con.execute(
                """
                UPDATE filing_context_backfill_builds
                SET status='completed',source_gap_row_count=?,queue_row_count=?,
                    ready_row_count=?,blocked_row_count=?,published_max_available_at=?,
                    published_content_hash=?,input_max_source_loaded_at=?,
                    completed_at=now(),source_loaded_at=now()
                WHERE build_id=?
                """,
                [
                    source_gap_row_count,
                    queue_row_count,
                    ready_row_count,
                    blocked_row_count,
                    max_available_at,
                    published_content_hash,
                    input_max_source_loaded_at,
                    build_id,
                ],
            )
        return FilingContextBackfillQueueResult(
            build_id=build_id,
            queue_row_count=queue_row_count,
            ready_row_count=ready_row_count,
            blocked_row_count=blocked_row_count,
            affected_reconciliation_count=affected_reconciliation_count,
            max_available_at=max_available_at,
            run_id=run_id,
        )
    except Exception as exc:
        store.con.execute(
            """
            UPDATE filing_context_backfill_builds
            SET status='failed',error_message=?,completed_at=now(),source_loaded_at=now()
            WHERE build_id=?
            """,
            [str(exc)[:2000], build_id],
        )
        raise
    finally:
        store.con.execute("DROP TABLE IF EXISTS filing_context_backfill_queue_next")


class FilingContextBackfillQueueDataset(Dataset):
    dataset_id = "filing_context_backfill_queue"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self,
        store: DuckDBStore,
        options: FilingContextBackfillQueueOptions,
    ) -> DatasetLoadResult:
        result = refresh_filing_context_backfill_queue(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="filing_context_backfill_queue",
            check_name="ready_rows_planned",
            status="passed" if result.ready_row_count > 0 else "warning",
            observed_value=float(result.ready_row_count),
            threshold_value=1.0,
            details={
                "build_id": result.build_id,
                "blocked_rows": result.blocked_row_count,
                "affected_reconciliations": result.affected_reconciliation_count,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=result.queue_row_count,
            source=self.source_name,
            run_id=result.run_id,
            details={
                "build_id": result.build_id,
                "ready_rows": result.ready_row_count,
                "blocked_rows": result.blocked_row_count,
                "affected_reconciliations": result.affected_reconciliation_count,
            },
        )
