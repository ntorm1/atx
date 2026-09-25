-- Q1: reported basic/diluted EPS, eight observed fiscal endpoints, as-of UTC.
-- Parameters: cutoff TIMESTAMP, cik VARCHAR (1-10 digits, zero-padded here).
-- Inspection, NOT IQ2 qualification.
-- Reuses standardized reported quarters and registry TTM states; never subtracts
-- annual/YTD EPS. YoY/QoQ comparisons match period geometry (end-date distance
-- and span length), never row offsets. SEC companyfacts fy/fp describe the
-- FILING, so a comparative re-report carries the later filing's labels: reported
-- fiscal labels come from the first visible revision and are diagnostics only.
-- All revision statistics below are recomputed inside the cutoff.
WITH p AS (
    SELECT $cutoff::TIMESTAMP AS cutoff,
           CASE WHEN regexp_full_match(trim($cik::VARCHAR),'[0-9]{1,10}')
                THEN lpad(trim($cik::VARCHAR),10,'0') END AS cik
), filings AS (
    SELECT f.cik, f.accession_number, min(f.filing_date) AS filing_date,
           max(f.acceptance_datetime) AS acceptance_datetime,
           count(DISTINCT f.acceptance_datetime) AS acceptance_versions
    FROM sec_submissions f, p WHERE f.cik=p.cik GROUP BY f.cik, f.accession_number
), visible AS (
    SELECT s.*, f.filing_date, f.acceptance_datetime, f.acceptance_versions,
           row_number() OVER w AS state_rank,
           count(*) OVER g AS visible_revision_count,
           count(*) OVER (PARTITION BY s.security_id,s.canonical_code,s.period_end,s.available_at)
               AS same_clock_candidates,
           first_value(s.value) OVER o AS first_visible_eps,
           first_value(s.fiscal_year IGNORE NULLS) OVER o AS original_fiscal_year,
           first_value(s.fiscal_period IGNORE NULLS) OVER o AS original_fiscal_period,
           min(s.available_at) OVER g AS first_visible_at
    FROM fundamental_standardized s CROSS JOIN p
    LEFT JOIN filings f ON f.cik=s.cik AND f.accession_number=s.source_accession
    WHERE s.cik=p.cik AND s.basis='quarterly'
      AND s.canonical_code IN ('eps_basic__1034','eps_diluted')
      AND s.available_at<=p.cutoff AND s.as_of_date<=p.cutoff::DATE
      AND s.period_end<=p.cutoff::DATE
      AND (f.acceptance_datetime IS NULL OR f.acceptance_datetime<=p.cutoff)
    -- Same precedence as the derived engine's state selection (_derived_pit).
    WINDOW g AS (PARTITION BY s.security_id,s.canonical_code,s.period_end),
           o AS (PARTITION BY s.security_id,s.canonical_code,s.period_end
                 ORDER BY s.available_at,s.standardized_id
                 ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING),
           w AS (PARTITION BY s.security_id,s.canonical_code,s.period_end
                 ORDER BY s.available_at DESC,s.source DESC,s.rule_id DESC,s.standardized_id DESC)
), selected AS (SELECT * FROM visible WHERE state_rank=1),
endpoints AS (
    SELECT DISTINCT period_end FROM selected ORDER BY period_end DESC LIMIT 8
), current_rows AS (SELECT c.* FROM selected c JOIN endpoints e USING (period_end)),
yoy AS (
    SELECT c.standardized_id,count(*) AS yoy_matches,
           CASE WHEN count(*)=1 THEN any_value(y.value) END AS yoy_base,
           CASE WHEN count(*)=1 THEN any_value(y.period_start) END AS yoy_start,
           CASE WHEN count(*)=1 THEN any_value(y.period_end) END AS yoy_end,
           CASE WHEN count(*)=1 THEN any_value(y.available_at) END AS yoy_base_available_at,
           CASE WHEN count(*)=1 THEN any_value(y.original_fiscal_year) END AS yoy_fiscal_year,
           CASE WHEN count(*)=1 THEN any_value(y.original_fiscal_period) END AS yoy_fiscal_period
    FROM current_rows c JOIN selected y ON y.security_id=c.security_id
      AND y.canonical_code=c.canonical_code
      AND date_diff('day',y.period_end,c.period_end) BETWEEN 350 AND 378
    GROUP BY c.standardized_id
), qoq AS (
    SELECT c.standardized_id,count(*) AS qoq_matches,
           CASE WHEN count(*)=1 THEN any_value(q.value) END AS qoq_base,
           CASE WHEN count(*)=1 THEN any_value(q.period_end) END AS qoq_end,
           CASE WHEN count(*)=1 THEN any_value(q.available_at) END AS qoq_base_available_at
    FROM current_rows c JOIN selected q ON q.security_id=c.security_id
      AND q.canonical_code=c.canonical_code
      AND q.period_end=c.period_start-INTERVAL 1 DAY
      AND date_diff('day',q.period_start,q.period_end)+1 BETWEEN 70 AND 110
    GROUP BY c.standardized_id
), ttm_ranked AS (
    SELECT d.*, row_number() OVER (PARTITION BY security_id,metric_code,period_end
              ORDER BY available_at DESC,derived_value_id DESC) AS state_rank
    FROM derived_metric_values d CROSS JOIN p
    WHERE d.source='atx-db declarative derived metrics v1'
      AND d.metric_code IN ('eps_basic_ttm','eps_diluted_ttm') AND d.metric_window='ttm'
      AND d.available_at<=p.cutoff AND d.as_of_date<=p.cutoff::DATE
      AND d.period_end IN (SELECT period_end FROM endpoints)
), paired AS (
    SELECT c.*, coalesce(y.yoy_matches,0) AS yoy_matches,y.yoy_base,y.yoy_start,y.yoy_end,
           y.yoy_fiscal_year,y.yoy_fiscal_period,
           coalesce(q.qoq_matches,0) AS qoq_matches,q.qoq_base,q.qoq_end,
           t.derived_value_id AS ttm_state_id,t.value AS stored_ttm_eps,
           t.value_status AS ttm_value_status,t.available_at AS ttm_available_at,
           t.history_status AS ttm_history_status,t.selected_input_refs_hash,
           greatest(c.available_at,y.yoy_base_available_at) AS yoy_available_at,
           greatest(c.available_at,q.qoq_base_available_at) AS qoq_available_at
    FROM current_rows c
    LEFT JOIN yoy y USING (standardized_id)
    LEFT JOIN qoq q USING (standardized_id)
    LEFT JOIN ttm_ranked t ON t.security_id=c.security_id AND t.period_end=c.period_end
      AND t.metric_code=CASE c.canonical_code WHEN 'eps_basic__1034' THEN 'eps_basic_ttm'
                                              ELSE 'eps_diluted_ttm' END
      AND t.state_rank=1
), classified AS (
    SELECT *,
      CASE WHEN period_start IS NULL OR date_diff('day',period_start,period_end)+1 NOT BETWEEN 70 AND 110
             THEN 'fiscal_span_unverified'
           WHEN yoy_matches<>1 OR yoy_start IS NULL THEN 'missing_or_ambiguous_comparison'
           WHEN abs(date_diff('day',yoy_start,yoy_end)-date_diff('day',period_start,period_end))>7
             THEN 'fiscal_year_change_or_stub'
           WHEN value IS NULL OR NOT isfinite(value) OR yoy_base IS NULL OR NOT isfinite(yoy_base)
             THEN 'missing_or_invalid_value'
           WHEN yoy_base=0 THEN 'zero_base'
           WHEN yoy_base<0 THEN 'negative_base_absolute_denominator' ELSE 'positive_base' END AS yoy_status,
      CASE WHEN yoy_matches<>1 THEN NULL
           WHEN original_fiscal_year IS NULL OR original_fiscal_period IS NULL
             OR yoy_fiscal_year IS NULL OR yoy_fiscal_period IS NULL THEN 'label_missing'
           WHEN yoy_fiscal_year=original_fiscal_year-1 AND yoy_fiscal_period=original_fiscal_period
             THEN 'labels_consistent'
           ELSE 'labels_differ_possible_fiscal_calendar_change' END AS yoy_label_status,
      CASE WHEN period_start IS NULL OR date_diff('day',period_start,period_end)+1 NOT BETWEEN 70 AND 110
             THEN 'fiscal_span_unverified'
           WHEN qoq_matches<>1 THEN 'missing_or_ambiguous_comparison'
           WHEN value IS NULL OR NOT isfinite(value) OR qoq_base IS NULL OR NOT isfinite(qoq_base)
             THEN 'missing_or_invalid_value'
           WHEN qoq_base=0 THEN 'zero_base'
           WHEN qoq_base<0 THEN 'negative_base_absolute_denominator' ELSE 'positive_base' END AS qoq_status
    FROM paired
)
SELECT cik,security_id AS issuer_owner_id,canonical_code AS eps_kind,
       original_fiscal_year AS fiscal_year,original_fiscal_period AS fiscal_period,
       fiscal_year AS selected_filing_fiscal_year,fiscal_period AS selected_filing_fiscal_period,
       period_start,period_end,value AS reported_quarter_eps,
       CASE WHEN ttm_value_status='valid' AND isfinite(stored_ttm_eps) THEN stored_ttm_eps END AS ttm_eps,
       ttm_state_id,ttm_value_status,ttm_available_at,ttm_history_status,selected_input_refs_hash,
       yoy_base,qoq_base,yoy_status,yoy_label_status,qoq_status,
       CASE WHEN yoy_status IN ('positive_base','negative_base_absolute_denominator')
            THEN (value-yoy_base)/abs(yoy_base) END AS yoy_growth,
       CASE WHEN qoq_status IN ('positive_base','negative_base_absolute_denominator')
            THEN (value-qoq_base)/abs(qoq_base) END AS qoq_growth,
       yoy_available_at,qoq_available_at,standardized_id,upstream_source,rule_id,
       same_clock_candidates,source_accession,
       filed_date,filing_date,acceptance_datetime,acceptance_versions,available_at,source_loaded_at,
       visible_revision_count,first_visible_eps,first_visible_at,
       value IS DISTINCT FROM first_visible_eps AS changed_since_first_visible,
       CASE WHEN acceptance_datetime IS NULL THEN 'modeled_or_unknown_acceptance'
            ELSE 'acceptance_present_not_delivery_vintage' END AS availability_status,
       count(DISTINCT security_id) OVER () AS visible_owner_count,
       count(DISTINCT period_end) OVER () AS observed_quarters,
       count(*) FILTER (WHERE canonical_code='eps_basic__1034')
           OVER (PARTITION BY security_id,period_end)=0 AS basic_missing_at_endpoint,
       count(*) FILTER (WHERE canonical_code='eps_diluted')
           OVER (PARTITION BY security_id,period_end)=0 AS diluted_missing_at_endpoint,
       false AS production_qualified,
       'inspection_requires_IQ2_selected_leaf_and_reported_quarter_proof' AS qualification
FROM classified ORDER BY period_end DESC,issuer_owner_id,eps_kind,standardized_id;
