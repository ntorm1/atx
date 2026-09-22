"""PIT bridge for accepted SEC reported quarterly diluted EPS."""

from __future__ import annotations

REPORTED_EPS_RELEASE_SOURCE = "SEC 8-K Item 2.02 reported earnings release"
COMPANYFACTS_SOURCE = "SEC companyfacts"
EPS_CONCEPT = "EarningsPerShareDiluted"
EPS_ITEM_ID = 1035
EPS_CONFLICT_TOLERANCE = 0.005


def reported_eps_source_facts_cte() -> str:
    """Project accepted releases under their exact CIK's single CF owner.

    The CTE preserves direct and release events independently.  Releases need
    explicit three-month evidence, or an explicit start for 13/14-week periods;
    it never derives a quarterly per-share value from annual or YTD figures.
    """

    return f"""
            source_facts AS (
                WITH release_windows AS (
                    SELECT
                        p.*,
                        lpad(trim(p.cik), 10, '0') AS normalized_cik,
                        json_extract_string(p.raw_payload_json, '$.duration_evidence') AS duration_evidence,
                        try_cast(json_extract_string(p.raw_payload_json, '$.period_start') AS DATE) AS explicit_period_start
                    FROM press_release_facts p
                    JOIN sec_earnings_release_receipts receipt
                      ON receipt.receipt_id = json_extract_string(p.raw_payload_json, '$.receipt_id')
                     AND receipt.cik = p.cik
                     AND receipt.accession_number = p.accession_number
                     AND receipt.document_sha256 = json_extract_string(p.raw_payload_json, '$.document_sha256')
                     AND receipt.source_security_id = p.security_id
                     AND receipt.available_at = p.available_at
                     AND receipt.outcome = 'accepted'
                    WHERE p.source = '{REPORTED_EPS_RELEASE_SOURCE}'
                      AND p.form = '8-K'
                      AND p.source_item = '2.02 / EX-99'
                      AND p.measure_code = 'EPS_DILUTED'
                      AND p.basis = 'GAAP'
                      AND p.unit = 'USD_PER_SHARE'
                      AND p.is_preliminary = true
                      AND p.extraction_confidence = 1.0
                      AND nullif(json_extract_string(p.raw_payload_json, '$.receipt_id'), '') IS NOT NULL
                      AND nullif(json_extract_string(p.raw_payload_json, '$.document_sha256'), '') IS NOT NULL
                      AND p.fiscal_year IS NOT NULL
                      AND p.fiscal_period IN ('Q1', 'Q2', 'Q3', 'Q4')
                      AND p.period_end IS NOT NULL
                      AND p.available_at IS NOT NULL
                      AND p.cik IS NOT NULL
                      AND regexp_full_match(trim(p.cik), '[0-9]{{1,10}}')
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
                ),
                companyfacts_owner AS (
                    SELECT lpad(trim(r.cik), 10, '0') AS cik,
                           min(r.security_id) AS accounting_owner
                    FROM fundamental_fact_revisions r
                    JOIN (SELECT DISTINCT normalized_cik FROM accepted_releases) p
                      ON lpad(trim(r.cik), 10, '0') = p.normalized_cik
                    WHERE r.source = '{COMPANYFACTS_SOURCE}'
                      AND r.cik IS NOT NULL
                      AND regexp_full_match(trim(r.cik), '[0-9]{{1,10}}')
                      AND r.security_id IS NOT NULL
                      AND trim(r.security_id) <> ''
                    GROUP BY lpad(trim(r.cik), 10, '0')
                    HAVING count(DISTINCT r.security_id) = 1
                )
                SELECT
                    r.fact_revision_id, r.revision_group_id, r.source,
                    r.security_id, r.cik, r.taxonomy, r.concept, r.unit,
                    r.period_start, r.period_end, r.accession_number,
                    r.filed_date, r.available_at, r.form, r.fiscal_year,
                    r.fiscal_period, r.frame, r.value, r.revision_sequence,
                    r.revision_count, r.is_latest_revision, r.is_value_changed,
                    r.previous_accession_number, r.previous_filed_date,
                    r.previous_available_at, r.previous_value, r.value_delta,
                    r.value_delta_percent, r.first_filed_date,
                    r.latest_filed_date, r.first_available_at,
                    r.latest_available_at, r.run_id, r.source_url,
                    r.source_loaded_at, r.updated_at, r.as_of_date
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
                    p.updated_at,
                    p.as_of_date
                FROM accepted_releases p
                JOIN companyfacts_owner owner ON owner.cik = p.normalized_cik
                WHERE p.period_start IS NOT NULL
                  -- The release parser supplies this label from the table header.
                  -- Keep the exact qualified 13/14-week spans; never admit a
                  -- generic 70--115-day release as a discrete EPS quarter.
                  AND (
                        (p.duration_evidence = 'three_months_explicit'
                         AND date_diff('day', p.period_start, p.period_end) + 1 BETWEEN 89 AND 93)
                     OR (p.duration_evidence = 'thirteen_weeks_explicit'
                         AND date_diff('day', p.period_start, p.period_end) + 1 = 91)
                     OR (p.duration_evidence = 'fourteen_weeks_explicit'
                         AND date_diff('day', p.period_start, p.period_end) + 1 = 98)
                  )
            )
    """


def reported_eps_conflict_candidates_cte() -> str:
    """Return NULL states that supersede a visible conflicting EPS release."""

    return f"""
            reported_eps_conflict_candidates AS (
                WITH release_eps AS (
                    SELECT
                        release.statement_point_id AS release_statement_point_id,
                        release.security_id,
                        lpad(trim(release.cik), 10, '0') AS cik,
                        release.item_id,
                        release.period_start,
                        release.period_end,
                        release.value,
                        release.available_at,
                        release.source_accession AS release_accession
                    FROM fundamental_statement_points release
                    WHERE release.source = '{REPORTED_EPS_RELEASE_SOURCE}'
                      AND release.taxonomy = 'us-gaap'
                      AND release.concept = '{EPS_CONCEPT}'
                      AND release.item_id = {EPS_ITEM_ID}
                      AND release.period_start IS NOT NULL
                      AND release.period_end IS NOT NULL
                      AND release.value IS NOT NULL
                      AND release.available_at IS NOT NULL
                      AND regexp_full_match(trim(release.cik), '[0-9]{{1,10}}')
                      AND date_diff('day', release.period_start, release.period_end) + 1 BETWEEN 70 AND 115
                ),
                direct_eps AS (
                    SELECT
                        direct.statement_point_id AS direct_statement_point_id,
                        direct.security_id,
                        lpad(trim(direct.cik), 10, '0') AS cik,
                        direct.symbol,
                        direct.item_id,
                        direct.canonical_metric,
                        'quarterly' AS basis,
                        direct.period_start,
                        direct.period_end,
                        direct.fiscal_year,
                        direct.fiscal_period,
                        direct.source_accession,
                        direct.filed_date,
                        direct.unit,
                        direct.unit_type,
                        direct.value,
                        direct.available_at,
                        direct.source_loaded_at
                    FROM fundamental_statement_points direct
                    WHERE direct.source = '{COMPANYFACTS_SOURCE}'
                      AND direct.taxonomy = 'us-gaap'
                      AND direct.concept = '{EPS_CONCEPT}'
                      AND direct.item_id = {EPS_ITEM_ID}
                      AND direct.period_start IS NOT NULL
                      AND direct.period_end IS NOT NULL
                      AND direct.value IS NOT NULL
                      AND direct.available_at IS NOT NULL
                      AND regexp_full_match(trim(direct.cik), '[0-9]{{1,10}}')
                      AND date_diff('day', direct.period_start, direct.period_end) + 1 BETWEEN 70 AND 115
                ),
                visible_pairs AS (
                    SELECT direct.*, release.release_statement_point_id,
                           release.release_accession,
                           release.value AS release_value,
                           direct.available_at AS conflict_available_at
                    FROM direct_eps direct
                    ASOF JOIN release_eps release
                      ON direct.security_id = release.security_id
                     AND direct.cik = release.cik
                     AND direct.item_id = release.item_id
                     AND direct.period_start = release.period_start
                     AND direct.period_end = release.period_end
                     AND direct.available_at >= release.available_at

                    UNION ALL

                    -- If a direct fact was visible first, a later release is
                    -- still a new source event. Compare the latest direct fact
                    -- at that release clock rather than silently replacing it.
                    SELECT direct.*, release.release_statement_point_id,
                           release.release_accession,
                           release.value AS release_value,
                           release.available_at AS conflict_available_at
                    FROM release_eps release
                    ASOF JOIN direct_eps direct
                      ON release.security_id = direct.security_id
                     AND release.cik = direct.cik
                     AND release.item_id = direct.item_id
                     AND release.period_start = direct.period_start
                     AND release.period_end = direct.period_end
                     AND release.available_at > direct.available_at
                )
                SELECT
                    'reported_eps_conflict' AS upstream_source,
                    10 AS upstream_priority,
                    sha256(concat_ws('|', 'reported_eps_conflict', direct_statement_point_id,
                                     release_statement_point_id)) AS upstream_row_id,
                    'reported_eps_conflict' AS upstream_adapter,
                    security_id,
                    symbol,
                    cik,
                    item_id,
                    canonical_metric,
                    -- Existing lineage fields remain the conflict audit
                    -- envelope: both source statement identities and
                    -- accessions stay in the standardized input code.
                    concat_ws(
                        '|',
                        'direct_statement_point_id=' || direct_statement_point_id,
                        'direct_accession=' || coalesce(source_accession, ''),
                        'release_statement_point_id=' || release_statement_point_id,
                        'release_accession=' || coalesce(release_accession, '')
                    ) AS concept,
                    'reported-eps-conflict' AS taxonomy,
                    unit,
                    unit_type,
                    basis,
                    period_start,
                    period_end,
                    fiscal_year,
                    fiscal_period,
                    source_accession AS accession_number,
                    source_accession,
                    filed_date,
                    CAST(NULL AS DOUBLE) AS value,
                    conflict_available_at AS available_at,
                    -1000 AS input_rank,
                    true AS source_is_latest,
                    source_loaded_at
                FROM visible_pairs
                WHERE abs(value - release_value) > {EPS_CONFLICT_TOLERANCE}
            )
    """
