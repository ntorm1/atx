"""Bounded core bridge for accepted SEC reported quarterly diluted EPS.

This relation deliberately does not manufacture a raw Company Facts revision.
It projects accepted ``press_release_facts`` only for the statement-point
materialization, keeping the original fact ID and raw CIK owner reachable in
the evidence table.  A release joins Company Facts only through an exact CIK
with one observed Company Facts accounting owner; it never consults a current
ticker or directory mapping.
"""

from __future__ import annotations

REPORTED_EPS_RELEASE_SOURCE = "SEC 8-K Item 2.02 reported earnings release"
COMPANYFACTS_SOURCE = "SEC companyfacts"
EPS_CONCEPT = "EarningsPerShareDiluted"
EPS_ITEM_ID = 1035
EPS_CONFLICT_TOLERANCE = 0.005


def reported_eps_source_facts_cte() -> str:
    """Return a SQL CTE named ``source_facts`` for statement-point refreshes.

    The resolver accepts only an explicitly labelled three-month release (or a
    13/14-week release with an explicit document start).  It projects both
    source events.  A direct Company Facts event that was already visible
    suppresses a later release projection; a later direct event remains a
    separate vintage for standardization to resolve.
    """

    return f"""
            source_facts AS (
                WITH companyfacts_owner AS (
                    SELECT lpad(trim(cik), 10, '0') AS cik, min(security_id) AS accounting_owner
                    FROM fundamental_fact_revisions
                    WHERE source = '{COMPANYFACTS_SOURCE}'
                      AND cik IS NOT NULL
                      AND trim(cik) <> ''
                      AND security_id IS NOT NULL
                      AND trim(security_id) <> ''
                    GROUP BY lpad(trim(cik), 10, '0')
                    HAVING count(DISTINCT security_id) = 1
                ),
                release_windows AS (
                    SELECT
                        p.*,
                        lpad(trim(p.cik), 10, '0') AS normalized_cik,
                        json_extract_string(p.raw_payload_json, '$.duration_evidence') AS duration_evidence,
                        try_cast(json_extract_string(p.raw_payload_json, '$.period_start') AS DATE) AS explicit_period_start
                    FROM press_release_facts p
                    WHERE p.source = '{REPORTED_EPS_RELEASE_SOURCE}'
                      AND p.form = '8-K'
                      AND p.source_item = '2.02 / EX-99'
                      AND p.measure_code = 'EPS_DILUTED'
                      AND p.basis = 'GAAP'
                      AND p.unit = 'USD_PER_SHARE'
                      AND p.fiscal_year IS NOT NULL
                      AND p.fiscal_period IN ('Q1', 'Q2', 'Q3', 'Q4')
                      AND p.period_end IS NOT NULL
                      AND p.available_at IS NOT NULL
                      AND p.cik IS NOT NULL
                      AND trim(p.cik) <> ''
                ),
                accepted_releases AS (
                    SELECT
                        p.*,
                        CASE
                            WHEN duration_evidence = 'three_months_explicit'
                                THEN CAST(p.period_end + INTERVAL 1 DAY - INTERVAL 3 MONTH AS DATE)
                            WHEN duration_evidence IN ('thirteen_weeks_explicit', 'fourteen_weeks_explicit')
                                THEN explicit_period_start
                        END AS period_start
                    FROM release_windows p
                )
                SELECT r.*
                FROM fundamental_fact_revisions r

                UNION ALL

                SELECT
                    p.press_release_fact_id AS fact_revision_id,
                    sha256(concat_ws('|', 'reported_eps_release', p.press_release_fact_id)) AS revision_group_id,
                    p.source,
                    owner.accounting_owner AS security_id,
                    p.normalized_cik AS cik,
                    'us-gaap' AS taxonomy,
                    '{EPS_CONCEPT}' AS concept,
                    'USD/shares' AS unit,
                    p.period_start,
                    p.period_end,
                    p.accession_number,
                    p.filing_date AS filed_date,
                    p.available_at,
                    p.form,
                    p.fiscal_year,
                    p.fiscal_period,
                    CAST(NULL AS VARCHAR) AS frame,
                    p.value,
                    1 AS revision_sequence,
                    1 AS revision_count,
                    true AS is_latest_revision,
                    false AS is_value_changed,
                    CAST(NULL AS VARCHAR) AS previous_accession_number,
                    CAST(NULL AS DATE) AS previous_filed_date,
                    CAST(NULL AS TIMESTAMP) AS previous_available_at,
                    CAST(NULL AS DOUBLE) AS previous_value,
                    CAST(NULL AS DOUBLE) AS value_delta,
                    CAST(NULL AS DOUBLE) AS value_delta_percent,
                    p.filing_date AS first_filed_date,
                    p.filing_date AS latest_filed_date,
                    p.available_at AS first_available_at,
                    p.available_at AS latest_available_at,
                    p.run_id,
                    p.source_url,
                    p.source_loaded_at,
                    p.updated_at
                FROM accepted_releases p
                JOIN companyfacts_owner owner ON owner.cik = p.normalized_cik
                WHERE p.period_start IS NOT NULL
                  AND date_diff('day', p.period_start, p.period_end) + 1 BETWEEN 70 AND 115
                  -- A Company Facts EPS already visible at this release event is
                  -- authoritative; do not publish a stale preliminary duplicate.
                  AND NOT EXISTS (
                      SELECT 1
                      FROM fundamental_fact_revisions direct
                      WHERE direct.source = '{COMPANYFACTS_SOURCE}'
                        AND lpad(trim(direct.cik), 10, '0') = p.normalized_cik
                        AND direct.taxonomy = 'us-gaap'
                        AND direct.concept = '{EPS_CONCEPT}'
                        AND direct.period_start = p.period_start
                        AND direct.period_end = p.period_end
                        AND direct.value IS NOT NULL
                        AND direct.available_at IS NOT NULL
                        AND direct.available_at <= p.available_at
                        AND date_diff('day', direct.period_start, direct.period_end) + 1 BETWEEN 70 AND 115
                  )
            )
    """
