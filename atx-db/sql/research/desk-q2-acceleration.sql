-- Q2: all visible derived owners, latest quarter vs its exact prior bucket.
-- Parameter: cutoff TIMESTAMP. Growth fractions; margin changes in fractions
-- (multiply by 100 for percentage points). Missing definitions stay visible.
-- Quarterly margin-change codes are desired contracts, currently unregistered.
-- Existing *_margin_change_yoy definitions are TTM and must not be relabeled Q.
-- Anchor = owner's latest visible bucket over ALL metrics (a newer invalid
-- quarter suppresses an older valid one); anchors >200 days old are stale.
WITH p AS (SELECT $cutoff::TIMESTAMP AS cutoff),
wanted(metric_code,metric_window) AS (VALUES
    ('eps_diluted_q_growth_yoy','q'),('gross_margin_q_change_yoy','q'),
    ('operating_margin_q_change_yoy','q')),
visible AS NOT MATERIALIZED (
    SELECT d.* FROM derived_metric_values d CROSS JOIN p
    WHERE source='atx-db declarative derived metrics v1' AND available_at<=p.cutoff
      AND as_of_date<=p.cutoff::DATE AND period_end<=p.cutoff::DATE
), anchors AS (
    SELECT security_id,max(target_bucket) AS target_bucket,max(period_end) AS anchor_period_end
    FROM visible GROUP BY security_id
), states AS (
    SELECT v.* FROM visible v JOIN wanted USING (metric_code,metric_window)
    QUALIFY row_number() OVER (PARTITION BY security_id,metric_code,metric_window,target_bucket
        ORDER BY available_at DESC,derived_value_id DESC)=1
),
joined AS (
    SELECT a.security_id AS issuer_owner_id,w.metric_code,w.metric_window,a.anchor_period_end,
           def.expression,def.version,
           c.derived_value_id AS current_state_id,b.derived_value_id AS prior_state_id,
           c.period_end,c.fiscal_period_start,c.fiscal_period_end,
           b.fiscal_period_start AS prior_start,b.fiscal_period_end AS prior_end,
           c.value AS latest_yoy,b.value AS prior_quarter_yoy,
           c.value_status AS current_value_status,b.value_status AS prior_value_status,
           c.available_at AS current_available_at,b.available_at AS prior_available_at,
           c.history_status,c.selected_input_refs_hash AS current_refs_hash,
           b.selected_input_refs_hash AS prior_refs_hash,
           c.selected_input_refs_json AS current_refs,b.selected_input_refs_json AS prior_refs,
           CASE WHEN def.metric_code IS NULL THEN 'metric_definition_missing'
                WHEN date_diff('day',a.anchor_period_end,p.cutoff::DATE)>200 THEN 'stale_latest_quarter'
                WHEN c.derived_value_id IS NULL OR b.derived_value_id IS NULL THEN 'missing_comparison'
                WHEN c.value_status IS DISTINCT FROM 'valid' OR b.value_status IS DISTINCT FROM 'valid'
                  OR c.value IS NULL OR b.value IS NULL OR NOT isfinite(c.value) OR NOT isfinite(b.value)
                  THEN 'missing_input_or_domain'
                WHEN c.fiscal_period_start IS NULL OR b.fiscal_period_start IS NULL
                  OR c.fiscal_period_end IS NULL OR b.fiscal_period_end IS NULL
                  THEN 'fiscal_span_unverified'
                WHEN c.fiscal_period_start<>b.fiscal_period_end+INTERVAL 1 DAY
                  OR date_diff('day',c.fiscal_period_start,c.fiscal_period_end)+1 NOT BETWEEN 70 AND 110
                  OR date_diff('day',b.fiscal_period_start,b.fiscal_period_end)+1 NOT BETWEEN 70 AND 110
                  THEN 'fiscal_year_change_gap_or_stub'
                ELSE 'arithmetic_candidate_fiscal_regime_and_lineage_unverified' END AS status
    FROM anchors a CROSS JOIN wanted w CROSS JOIN p
    LEFT JOIN derived_metric_definitions def ON def.metric_code=w.metric_code AND def.metric_window=w.metric_window
    LEFT JOIN states c ON c.security_id=a.security_id AND c.target_bucket=a.target_bucket
      AND c.metric_code=w.metric_code AND c.metric_window=w.metric_window
    LEFT JOIN states b ON b.security_id=a.security_id AND b.target_bucket=a.target_bucket-1
      AND b.metric_code=w.metric_code AND b.metric_window=w.metric_window
), bases AS (
    SELECT j.*,cb.base AS latest_yoy_eps_base,pb.base AS prior_yoy_eps_base
    FROM joined j
    LEFT JOIN LATERAL (
        SELECT CASE WHEN count(*)=1 THEN max(s.value) END AS base
        FROM json_each(CASE WHEN json_valid(j.current_refs) THEN j.current_refs
                       ELSE '{"refs":[]}' END,'$.refs') r
        JOIN fundamental_standardized s ON s.standardized_id=json_extract_string(r.value,'$.state_id')
        WHERE json_extract_string(r.value,'$.kind')='item'
          AND json_extract_string(r.value,'$.code') IN ('eps_diluted','eps_diluted__1035')
          AND try_cast(json_extract_string(r.value,'$.offset') AS INTEGER)=4
          AND s.available_at<=j.current_available_at
    ) cb ON true
    LEFT JOIN LATERAL (
        SELECT CASE WHEN count(*)=1 THEN max(s.value) END AS base
        FROM json_each(CASE WHEN json_valid(j.prior_refs) THEN j.prior_refs
                       ELSE '{"refs":[]}' END,'$.refs') r
        JOIN fundamental_standardized s ON s.standardized_id=json_extract_string(r.value,'$.state_id')
        WHERE json_extract_string(r.value,'$.kind')='item'
          AND json_extract_string(r.value,'$.code') IN ('eps_diluted','eps_diluted__1035')
          AND try_cast(json_extract_string(r.value,'$.offset') AS INTEGER)=4
          AND s.available_at<=j.prior_available_at
    ) pb ON true
)
SELECT * EXCLUDE (current_refs,prior_refs),
       CASE WHEN status='arithmetic_candidate_fiscal_regime_and_lineage_unverified'
            THEN latest_yoy-prior_quarter_yoy END AS acceleration,
       greatest(current_available_at,prior_available_at) AS acceleration_available_at,
       CASE WHEN metric_code<>'eps_diluted_q_growth_yoy' THEN 'not_applicable'
            WHEN latest_yoy_eps_base IS NULL OR prior_yoy_eps_base IS NULL THEN 'base_leaf_missing_or_ambiguous'
            WHEN latest_yoy_eps_base=0 OR prior_yoy_eps_base=0 THEN 'zero_base_growth_unavailable'
            WHEN latest_yoy_eps_base<0 OR prior_yoy_eps_base<0 THEN 'negative_base_absolute_denominator'
            ELSE 'positive_bases' END AS eps_base_status,
       'EPS=(current-base)/abs(base); zero/missing base=NULL; bases inspected, selected-leaf proof still required'
           AS negative_base_policy,
       current_refs IS NOT NULL AND prior_refs IS NOT NULL AS selected_refs_present,
       count(DISTINCT issuer_owner_id) OVER () AS visible_owner_count,
       false AS production_qualified
FROM bases ORDER BY issuer_owner_id,metric_code;
