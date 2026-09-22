-- Read-only quarterly reported-EPS / YoY acceptance query.
--
-- Edit only operator_input for a different issuer, cutoff, or fiscal calendar.
-- Each row supplies the exact current and same-fiscal-quarter-prior windows;
-- the SQL never infers the prior period by subtracting one calendar year.
--
-- CURRENT-SCHEMA INPUTS ONLY:
--   sec_company_facts        direct Company Facts EPS evidence
--   derived_metric_values    published quarterly YoY materialization
--
-- This intentionally does not construct a Q4 value from FY / YTD EPS.

WITH operator_input AS (
    SELECT *
    FROM (VALUES
        -- cik, cutoff, label, current quarter [start,end], prior quarter [start,end]
        ('0000093410', TIMESTAMP '2026-09-20 22:00:00', 'Q2 2026',
         DATE '2026-04-01', DATE '2026-06-30', DATE '2025-04-01', DATE '2025-06-30'),
        ('0000093410', TIMESTAMP '2026-09-20 22:00:00', 'Q1 2026',
         DATE '2026-01-01', DATE '2026-03-31', DATE '2025-01-01', DATE '2025-03-31'),
        ('0000093410', TIMESTAMP '2026-09-20 22:00:00', 'Q4 2025',
         DATE '2025-10-01', DATE '2025-12-31', DATE '2024-10-01', DATE '2024-12-31')
    ) AS t(
        cik, cutoff, requested_quarter,
        current_period_start, current_period_end,
        prior_period_start, prior_period_end
    )
),
requested_inputs AS (
    SELECT cik, cutoff, requested_quarter, 'current' AS input_role,
           current_period_start AS period_start, current_period_end AS period_end
    FROM operator_input
    UNION ALL
    SELECT cik, cutoff, requested_quarter, 'prior' AS input_role,
           prior_period_start AS period_start, prior_period_end AS period_end
    FROM operator_input
),
raw_matching_window AS (
    SELECT
        r.cik,
        i.requested_quarter,
        i.input_role,
        i.cutoff,
        r.security_id,
        r.period_start,
        r.period_end,
        r.value,
        r.unit,
        r.fiscal_year,
        r.fiscal_period,
        r.form,
        r.filed_date,
        r.accession_number,
        r.available_at AS stored_available_at,
        greatest(r.available_at, CAST(r.filed_date AS TIMESTAMP) + INTERVAL 46 HOUR)
            AS effective_available_at,
        r.source_loaded_at,
        CASE
            WHEN greatest(r.available_at, CAST(r.filed_date AS TIMESTAMP) + INTERVAL 46 HOUR)
                 <= i.cutoff
            THEN true ELSE false
        END AS is_visible_at_cutoff
    FROM requested_inputs i
    JOIN sec_company_facts r
      ON r.cik = i.cik
     AND r.period_start = i.period_start
     AND r.period_end = i.period_end
    WHERE r.source = 'SEC companyfacts'
      AND r.taxonomy = 'us-gaap'
      AND r.concept = 'EarningsPerShareDiluted'
      AND lower(coalesce(r.unit, '')) IN ('usd/shares', 'usd/share', 'usd_per_share')
),
raw_window_counts AS (
    SELECT
        i.cik, i.cutoff, i.requested_quarter, i.input_role,
        count(r.accession_number) AS matching_window_rows,
        count(r.accession_number) FILTER (WHERE r.is_visible_at_cutoff) AS visible_window_rows
    FROM requested_inputs i
    LEFT JOIN raw_matching_window r
      ON r.cik = i.cik
     AND r.requested_quarter = i.requested_quarter
     AND r.input_role = i.input_role
    GROUP BY 1, 2, 3, 4
),
raw_visible_ranked AS (
    SELECT
        r.*,
        row_number() OVER (
            PARTITION BY r.cik, r.cutoff, r.requested_quarter, r.input_role
            ORDER BY r.filed_date DESC,
                     r.effective_available_at DESC,
                     r.source_loaded_at DESC,
                     r.accession_number DESC
        ) AS rn
    FROM raw_matching_window r
    WHERE r.is_visible_at_cutoff
),
selected_raw AS (
    SELECT * FROM raw_visible_ranked WHERE rn = 1
),
raw_pair AS (
    SELECT
        q.*,
        cur.security_id AS current_security_id,
        cur.value AS current_reported_eps,
        cur.unit AS current_unit,
        cur.fiscal_year AS current_fiscal_year,
        cur.fiscal_period AS current_fiscal_period,
        cur.form AS current_form,
        cur.filed_date AS current_filed_date,
        cur.accession_number AS current_accession_number,
        cur.stored_available_at AS current_stored_available_at,
        cur.effective_available_at AS current_effective_available_at,
        cur.source_loaded_at AS current_source_loaded_at,
        prv.security_id AS prior_security_id,
        prv.value AS prior_reported_eps,
        prv.unit AS prior_unit,
        prv.fiscal_year AS prior_fiscal_year,
        prv.fiscal_period AS prior_fiscal_period,
        prv.form AS prior_form,
        prv.filed_date AS prior_filed_date,
        prv.accession_number AS prior_accession_number,
        prv.stored_available_at AS prior_stored_available_at,
        prv.effective_available_at AS prior_effective_available_at,
        prv.source_loaded_at AS prior_source_loaded_at,
        cur_count.matching_window_rows AS current_matching_window_rows,
        cur_count.visible_window_rows AS current_visible_window_rows,
        prv_count.matching_window_rows AS prior_matching_window_rows,
        prv_count.visible_window_rows AS prior_visible_window_rows
    FROM operator_input q
    LEFT JOIN selected_raw cur
      ON cur.cik = q.cik
     AND cur.cutoff = q.cutoff
     AND cur.requested_quarter = q.requested_quarter
     AND cur.input_role = 'current'
    LEFT JOIN selected_raw prv
      ON prv.cik = q.cik
     AND prv.cutoff = q.cutoff
     AND prv.requested_quarter = q.requested_quarter
     AND prv.input_role = 'prior'
    LEFT JOIN raw_window_counts cur_count
      ON cur_count.cik = q.cik
     AND cur_count.cutoff = q.cutoff
     AND cur_count.requested_quarter = q.requested_quarter
     AND cur_count.input_role = 'current'
    LEFT JOIN raw_window_counts prv_count
      ON prv_count.cik = q.cik
     AND prv_count.cutoff = q.cutoff
     AND prv_count.requested_quarter = q.requested_quarter
     AND prv_count.input_role = 'prior'
),
eligible_cik_owner_ids AS (
    -- Do not require a direct EPS fact for the target quarter.  A release-fed
    -- Q4 can materialize under the issuer's raw CIK owner even when the direct
    -- Company Facts Q4 input is absent.
    SELECT DISTINCT
        i.cik,
        i.cutoff,
        f.security_id
    FROM operator_input i
    JOIN sec_company_facts f
      ON f.cik = i.cik
    WHERE f.source = 'SEC companyfacts'
      AND f.security_id IS NOT NULL
      AND greatest(f.available_at, CAST(f.filed_date AS TIMESTAMP) + INTERVAL 46 HOUR)
          <= i.cutoff
),
derived_visible_ranked AS (
    -- Rank visible states only after retaining NULL/unavailable states.  This
    -- avoids turning a published unavailable state into apparent absence.
    SELECT
        q.cik AS requested_cik,
        q.cutoff AS requested_cutoff,
        q.requested_quarter,
        d.*,
        row_number() OVER (
            PARTITION BY q.cik, q.cutoff, q.requested_quarter, d.security_id, d.period_end,
                         coalesce(d.revision_group_id, d.derived_value_id)
            ORDER BY d.available_at DESC, d.derived_value_id DESC
        ) AS revision_rn
    FROM derived_metric_values d
    JOIN operator_input q
      ON d.period_end = q.current_period_end
    JOIN eligible_cik_owner_ids p
      ON p.cik = q.cik
     AND p.cutoff = q.cutoff
     AND p.security_id = d.security_id
     AND d.available_at <= q.cutoff
    WHERE d.source = 'atx-db declarative derived metrics v1'
      AND d.metric_code = 'eps_diluted_q_growth_yoy'
      AND d.metric_window = 'q'
),
derived_latest AS (
    SELECT * FROM derived_visible_ranked WHERE revision_rn = 1
),
derived_for_target AS (
    SELECT
        r.cik,
        r.cutoff,
        r.requested_quarter,
        d.derived_value_id,
        d.revision_group_id,
        d.value AS published_quarterly_eps_yoy,
        d.value_status AS published_value_status,
        d.value_origin AS published_value_origin,
        d.available_at AS published_available_at,
        d.source_loaded_at AS published_source_loaded_at,
        row_number() OVER (
            PARTITION BY r.cik, r.cutoff, r.requested_quarter
            ORDER BY d.available_at DESC, d.derived_value_id DESC
        ) AS target_rn
    FROM raw_pair r
    LEFT JOIN derived_latest d
      ON d.requested_cik = r.cik
     AND d.requested_cutoff = r.cutoff
     AND d.requested_quarter = r.requested_quarter
     AND d.period_end = r.current_period_end
)
SELECT
    r.cik,
    r.cutoff,
    r.requested_quarter,
    r.current_period_start,
    r.current_period_end,
    r.prior_period_start,
    r.prior_period_end,
    r.current_reported_eps,
    r.prior_reported_eps,
    CASE
        WHEN r.current_reported_eps IS NOT NULL
         AND r.prior_reported_eps IS NOT NULL
         AND isfinite(r.current_reported_eps)
         AND isfinite(r.prior_reported_eps)
         AND r.prior_reported_eps <> 0
        THEN (r.current_reported_eps - r.prior_reported_eps) / abs(r.prior_reported_eps)
        ELSE NULL
    END AS raw_computed_quarterly_eps_yoy,
    d.published_quarterly_eps_yoy,
    d.published_value_status,
    d.published_value_origin,
    d.derived_value_id,
    d.revision_group_id AS published_revision_group_id,
    d.published_available_at,
    d.published_source_loaded_at,
    CASE
        WHEN r.current_reported_eps IS NULL AND r.current_matching_window_rows = 0
            THEN 'raw_current_eps_missing'
        WHEN r.current_reported_eps IS NULL
            THEN 'raw_current_eps_not_visible_by_cutoff'
        WHEN r.prior_reported_eps IS NULL AND r.prior_matching_window_rows = 0
            THEN 'raw_prior_eps_missing'
        WHEN r.prior_reported_eps IS NULL
            THEN 'raw_prior_eps_not_visible_by_cutoff'
        WHEN NOT isfinite(r.current_reported_eps)
            THEN 'raw_current_eps_nonfinite'
        WHEN NOT isfinite(r.prior_reported_eps)
            THEN 'raw_prior_eps_nonfinite'
        WHEN r.prior_reported_eps = 0
            THEN 'raw_prior_eps_zero_yoy_undefined'
        ELSE 'raw_inputs_present'
    END AS raw_input_status,
    CASE
        WHEN r.current_reported_eps IS NULL OR r.prior_reported_eps IS NULL
            THEN 'not_evaluable_raw_input_missing'
        WHEN NOT isfinite(r.current_reported_eps) OR NOT isfinite(r.prior_reported_eps)
            THEN 'not_evaluable_nonfinite_raw_input'
        WHEN r.prior_reported_eps = 0
            THEN 'not_evaluable_prior_zero'
        WHEN d.derived_value_id IS NULL
            THEN 'materialization_missing'
        WHEN d.published_value_status IS DISTINCT FROM 'valid'
            THEN 'materialized_unavailable_or_invalid'
        WHEN d.published_quarterly_eps_yoy IS NULL
            THEN 'materialized_null'
        ELSE 'published_valid'
    END AS published_metric_status,
    CASE
        WHEN d.derived_value_id IS NULL
            THEN 'no_visible_published_metric_for_eligible_cik_owner'
        WHEN d.published_available_at > r.cutoff
            THEN 'published_metric_not_visible_by_cutoff'
        ELSE 'published_metric_visible'
    END AS published_availability_status,
    r.current_matching_window_rows,
    r.current_visible_window_rows,
    r.prior_matching_window_rows,
    r.prior_visible_window_rows,
    r.current_security_id,
    r.current_unit,
    r.current_fiscal_year,
    r.current_fiscal_period,
    r.current_form,
    r.current_filed_date,
    r.current_accession_number,
    r.current_stored_available_at,
    r.current_effective_available_at,
    r.current_source_loaded_at,
    r.prior_security_id,
    r.prior_unit,
    r.prior_fiscal_year,
    r.prior_fiscal_period,
    r.prior_form,
    r.prior_filed_date,
    r.prior_accession_number,
    r.prior_stored_available_at,
    r.prior_effective_available_at,
    r.prior_source_loaded_at
FROM raw_pair r
LEFT JOIN derived_for_target d
  ON d.cik = r.cik
 AND d.cutoff = r.cutoff
 AND d.requested_quarter = r.requested_quarter
 AND d.target_rn = 1
ORDER BY r.current_period_end DESC;
