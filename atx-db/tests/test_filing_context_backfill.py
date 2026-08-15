from __future__ import annotations

from atx_db.filing_context_backfill import (
    FilingContextBackfillQueueOptions,
    refresh_filing_context_backfill_queue,
)
from atx_db.quality import run_warehouse_quality_checks


def _seed_reconciliation_build(store, build_id: str = "reconciliation-build-1", content_hash: int = 123) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_reconciliation_builds (
            build_id,status,scope_json,started_at,completed_at,run_id,
            is_full_refresh,published_content_hash
        ) VALUES (?,'completed','{"symbols":null}',TIMESTAMP '2026-08-14 10:00:00',
                  TIMESTAMP '2026-08-14 10:01:00','reconciliation-run',true,?)
        """,
        [build_id, content_hash],
    )


def _insert_reconciliation_gap(
    store,
    *,
    reconciliation_id: str,
    security_id: str,
    symbol: str,
    cik: str,
    accession_number: str,
    rule_id: str,
    status: str,
    period_end: str,
    absolute_difference: float,
) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_reconciliation_serving (
            reconciliation_id,security_id,symbol,cik,rule_id,period_end,
            mismatch_severity,is_applicable,input_filing_status,
            input_accession_count,input_accessions_json,status,
            absolute_difference,residual_percent,context_verification_status,
            is_latest_revision,available_at,source_loaded_at
        ) VALUES (?,?,?,?,?,CAST(? AS DATE),'error',true,'single_filing',1,?,?,?,0.01,
                  'context_not_loaded',true,TIMESTAMP '2026-08-14 09:00:00',
                  TIMESTAMP '2026-08-14 09:05:00')
        """,
        [
            reconciliation_id,
            security_id,
            symbol,
            cik,
            rule_id,
            period_end,
            f'["{accession_number}"]',
            status,
            absolute_difference,
        ],
    )


def _insert_submission(
    store,
    *,
    security_id: str,
    cik: str,
    accession_number: str,
    primary_document: str,
    is_inline_xbrl: bool,
    filing_date: str,
) -> None:
    store.con.execute(
        """
        INSERT INTO sec_submissions (
            security_id,cik,accession_number,filing_date,report_date,
            acceptance_datetime,form,primary_document,size,is_xbrl,is_inline_xbrl,
            source_url,source_loaded_at
        ) VALUES (?,?,?,CAST(? AS DATE),CAST(? AS DATE),
                  TIMESTAMP '2026-08-14 08:00:00','10-K',?,1000000,true,?,
                  'https://data.sec.gov/submissions/fixture.json',
                  TIMESTAMP '2026-08-14 08:05:00')
        """,
        [
            security_id,
            cik,
            accession_number,
            filing_date,
            filing_date,
            primary_document,
            is_inline_xbrl,
        ],
    )


def _seed_queue_inputs(store) -> None:
    _seed_reconciliation_build(store)
    _insert_reconciliation_gap(
        store,
        reconciliation_id="aapl-diagnostic",
        security_id="SEC-CIK-0000320193",
        symbol="AAPL",
        cik="0000320193",
        accession_number="0000320193-26-000001",
        rule_id="assets_equal_liabilities_equity_instant",
        status="diagnostic_difference",
        period_end="2025-12-31",
        absolute_difference=500.0,
    )
    _insert_reconciliation_gap(
        store,
        reconciliation_id="aapl-reconciled",
        security_id="SEC-CIK-0000320193",
        symbol="AAPL",
        cik="0000320193",
        accession_number="0000320193-26-000001",
        rule_id="cash_rollforward_annual",
        status="reconciled",
        period_end="2025-12-31",
        absolute_difference=0.0,
    )
    _insert_reconciliation_gap(
        store,
        reconciliation_id="aig-reconciled",
        security_id="SEC-CIK-0000005272",
        symbol="AIG",
        cik="0000005272",
        accession_number="0001047469-12-005310",
        rule_id="assets_equal_liabilities_equity_instant",
        status="reconciled",
        period_end="2012-03-31",
        absolute_difference=0.0,
    )
    _insert_reconciliation_gap(
        store,
        reconciliation_id="msft-blocked",
        security_id="SEC-CIK-0000789019",
        symbol="MSFT",
        cik="0000789019",
        accession_number="0000789019-26-999999",
        rule_id="assets_equal_liabilities_equity_instant",
        status="diagnostic_difference",
        period_end="2025-12-31",
        absolute_difference=100.0,
    )
    _insert_submission(
        store,
        security_id="SEC-CIK-0000320193",
        cik="0000320193",
        accession_number="0000320193-26-000001",
        primary_document="aapl-20251231.htm",
        is_inline_xbrl=True,
        filing_date="2026-01-31",
    )
    _insert_submission(
        store,
        security_id="SEC-CIK-0000005272",
        cik="0000005272",
        accession_number="0001047469-12-005310",
        primary_document="aig-20120331x10q.htm",
        is_inline_xbrl=False,
        filing_date="2012-05-03",
    )


def test_refresh_queue_groups_accessions_and_prioritizes_verification_gain(tmp_store):
    _seed_queue_inputs(tmp_store)

    result = refresh_filing_context_backfill_queue(
        tmp_store,
        FilingContextBackfillQueueOptions(run_id="queue-test-run"),
    )

    assert result.queue_row_count == 3
    assert result.ready_row_count == 2
    assert result.blocked_row_count == 1
    assert result.affected_reconciliation_count == 4
    rows = tmp_store.con.execute(
        """
        SELECT
            priority_rank,symbol,queue_status,blocked_reason,priority_tier,
            expected_instance_format,estimated_request_count,
            affected_reconciliation_count,filing_index_url,primary_document_url
        FROM filing_context_backfill_queue
        ORDER BY priority_rank
        """
    ).fetchall()
    assert rows[0][:8] == (1, "AAPL", "pending", None, "P1", "inline_xbrl", 1, 2)
    assert rows[0][8].endswith("/000032019326000001/index.json")
    assert rows[0][9].endswith("/000032019326000001/aapl-20251231.htm")
    assert rows[1][:8] == (2, "AIG", "pending", None, "P2", "xbrl_xml", 2, 1)
    assert rows[2][:8] == (
        3,
        "MSFT",
        "blocked",
        "missing_sec_submission",
        "P1",
        None,
        0,
        1,
    )


def test_queue_refresh_is_repeatable_and_manifest_matches_publication(tmp_store):
    _seed_queue_inputs(tmp_store)
    first = refresh_filing_context_backfill_queue(tmp_store)
    second = refresh_filing_context_backfill_queue(tmp_store)

    assert first.queue_row_count == second.queue_row_count == 3
    assert tmp_store.con.execute(
        "SELECT count(*) FROM filing_context_backfill_builds WHERE status='completed'"
    ).fetchone() == (2,)
    assert tmp_store.con.execute(
        "SELECT count(DISTINCT build_id),min(build_id) FROM filing_context_backfill_queue"
    ).fetchone() == (1, second.build_id)
    checks = run_warehouse_quality_checks(
        tmp_store,
        dataset_ids=("filing_context_backfill_queue",),
        record=False,
    )
    by_name = {check.check_name: check for check in checks}
    assert by_name["bad_filing_context_backfill_queue_rows"].status == "passed"
    assert by_name["duplicate_filing_context_backfill_accessions"].status == "passed"
    assert by_name["filing_context_backfill_queue_manifest_parity"].status == "passed"
    assert by_name["filing_context_backfill_queue_freshness"].status == "passed"
    assert by_name["blocked_filing_context_backfill_queue_rows"].status == "warning"


def test_queue_freshness_detects_newer_submission_metadata(tmp_store):
    _seed_queue_inputs(tmp_store)
    refresh_filing_context_backfill_queue(tmp_store)

    tmp_store.con.execute(
        """
        UPDATE sec_submissions
        SET source_loaded_at=TIMESTAMP '2026-08-14 11:00:00'
        WHERE accession_number='0000320193-26-000001'
        """
    )
    stale_checks = run_warehouse_quality_checks(
        tmp_store,
        dataset_ids=("filing_context_backfill_queue",),
        record=False,
    )
    stale_by_name = {check.check_name: check for check in stale_checks}
    assert stale_by_name["filing_context_backfill_queue_freshness"].status == "failed"

    refresh_filing_context_backfill_queue(tmp_store)
    refreshed_checks = run_warehouse_quality_checks(
        tmp_store,
        dataset_ids=("filing_context_backfill_queue",),
        record=False,
    )
    refreshed_by_name = {check.check_name: check for check in refreshed_checks}
    assert refreshed_by_name["filing_context_backfill_queue_freshness"].status == "passed"


def test_queue_quality_detects_tampering_and_new_reconciliation_build(tmp_store):
    _seed_queue_inputs(tmp_store)
    refresh_filing_context_backfill_queue(tmp_store)
    tmp_store.con.execute(
        """
        UPDATE filing_context_backfill_queue
        SET blocked_reason='tampered'
        WHERE queue_status='pending' AND priority_rank=1
        """
    )
    _seed_reconciliation_build(tmp_store, "reconciliation-build-2", 456)

    checks = run_warehouse_quality_checks(
        tmp_store,
        dataset_ids=("filing_context_backfill_queue",),
        record=False,
    )
    by_name = {check.check_name: check for check in checks}
    assert by_name["bad_filing_context_backfill_queue_rows"].status == "failed"
    assert by_name["filing_context_backfill_queue_manifest_parity"].status == "failed"
    assert by_name["filing_context_backfill_queue_freshness"].status == "failed"


def test_empty_queue_build_is_fresh_on_empty_reconciliation_surface(tmp_store):
    result = refresh_filing_context_backfill_queue(tmp_store)
    assert result.queue_row_count == 0
    checks = run_warehouse_quality_checks(
        tmp_store,
        dataset_ids=("filing_context_backfill_queue",),
        record=False,
    )
    assert all(check.status == "passed" for check in checks)
