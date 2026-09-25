-- Q2: all visible derived owners, latest-quarter YoY growth, the prior bucket's
-- YoY growth and the REGISTERED acceleration (A4 quarter-grid contracts).
-- Parameter: cutoff TIMESTAMP. Growth in fractions; margin changes and their
-- accelerations in fraction points (multiply by 100 for percentage points).
-- Families (growth code -> registered acceleration code, window q):
--   eps_diluted_q_growth_yoy      -> eps_diluted_q_growth_yoy_accel
--   gross_margin_q_change_yoy     -> gross_margin_q_change_yoy_accel
--   operating_margin_q_change_yoy -> operating_margin_q_change_yoy_accel
-- The trailing *_margin_change_yoy (ttm) definitions are never relabeled Q.
-- acceleration = the registered state's value g(t)-g(t-1), shown only when both
-- growth states are valid on the exact anchor and prior buckets, the engine
-- proved one-quarter fiscal adjacency (value_origin='quarterly': 70-120 day
-- spans, gap <= 7 days; stubs and fiscal-year changes stay incomparable), and
-- the registered value equals current minus prior growth (lineage check).
-- Missing definitions/states stay visible; nothing is recomputed ad hoc.
-- Anchor = owner's latest visible bucket over ALL metrics (a newer invalid
-- quarter suppresses an older valid one); anchors >200 days old are stale.
WITH p AS (SELECT $cutoff::TIMESTAMP AS cutoff),
families(metric_code,accel_code,metric_window) AS (VALUES
    ('eps_diluted_q_growth_yoy','eps_diluted_q_growth_yoy_accel','q'),
    ('gross_margin_q_change_yoy','gross_margin_q_change_yoy_accel','q'),
    ('operating_margin_q_change_yoy','operating_margin_q_change_yoy_accel','q')),
wanted AS (
    SELECT metric_code,metric_window FROM families
    UNION ALL SELECT accel_code,metric_window FROM families
),
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
    SELECT a.security_id AS issuer_owner_id,f.metric_code,f.accel_code,f.metric_window,
           a.target_bucket AS anchor_bucket,a.anchor_period_end,
           def.version AS growth_definition_version,adef.version AS accel_definition_version,
           adef.expression AS accel_expression,
           c.derived_value_id AS current_state_id,b.derived_value_id AS prior_state_id,
           x.derived_value_id AS accel_state_id,
           c.period_end,c.fiscal_period_start,c.fiscal_period_end,
           b.fiscal_period_start AS prior_start,b.fiscal_period_end AS prior_end,
           date_diff('day',b.fiscal_period_end,c.fiscal_period_start) AS span_gap_days,
           c.value AS latest_yoy,b.value AS prior_quarter_yoy,
           c.value_status AS current_value_status,b.value_status AS prior_value_status,
           c.value_origin AS current_value_origin,b.value_origin AS prior_value_origin,
           x.value_status AS accel_value_status,x.value_origin AS accel_value_origin,
           c.available_at AS current_available_at,b.available_at AS prior_available_at,
           x.available_at AS accel_available_at,
           c.history_status,c.selected_input_refs_hash AS current_refs_hash,
           b.selected_input_refs_hash AS prior_refs_hash,x.selected_input_refs_hash AS accel_refs_hash,
           c.selected_input_refs_json AS current_refs,b.selected_input_refs_json AS prior_refs,
           x.value AS registered_accel,
           CASE WHEN def.metric_code IS NULL OR adef.metric_code IS NULL THEN 'metric_definition_missing'
                WHEN date_diff('day',a.anchor_period_end,p.cutoff::DATE)>200 THEN 'stale_latest_quarter'
                WHEN c.derived_value_id IS NULL OR b.derived_value_id IS NULL THEN 'missing_comparison'
                WHEN c.value_status IS DISTINCT FROM 'valid' OR b.value_status IS DISTINCT FROM 'valid'
                  OR c.value IS NULL OR b.value IS NULL OR NOT isfinite(c.value) OR NOT isfinite(b.value)
                  THEN 'missing_input_or_domain'
                WHEN x.derived_value_id IS NULL THEN 'accel_state_missing'
                WHEN x.value_status IS DISTINCT FROM 'valid' OR x.value IS NULL OR NOT isfinite(x.value)
                  THEN 'accel_missing_input_or_domain'
                -- The engine propagates incomparable from an incomparable growth input
                -- (e.g. per-share YoY without a proven split basis): name that apart
                -- from a one-quarter fiscal-adjacency failure.
                WHEN x.value_origin IS DISTINCT FROM 'quarterly' THEN
                     CASE WHEN c.value_origin IS DISTINCT FROM 'quarterly' OR b.value_origin IS DISTINCT FROM 'quarterly'
                          THEN 'growth_origin_not_quarterly' ELSE 'accel_adjacency_unproven' END
                WHEN abs(x.value-(c.value-b.value))>1e-9*greatest(1.0,abs(x.value))
                  THEN 'accel_growth_state_mismatch'
                ELSE 'registered_candidate_lineage_unverified' END AS status
    FROM anchors a CROSS JOIN families f CROSS JOIN p
    LEFT JOIN derived_metric_definitions def ON def.metric_code=f.metric_code AND def.metric_window=f.metric_window
    LEFT JOIN derived_metric_definitions adef ON adef.metric_code=f.accel_code AND adef.metric_window=f.metric_window
    LEFT JOIN states c ON c.security_id=a.security_id AND c.target_bucket=a.target_bucket
      AND c.metric_code=f.metric_code AND c.metric_window=f.metric_window
    LEFT JOIN states b ON b.security_id=a.security_id AND b.target_bucket=a.target_bucket-1
      AND b.metric_code=f.metric_code AND b.metric_window=f.metric_window
    LEFT JOIN states x ON x.security_id=a.security_id AND x.target_bucket=a.target_bucket
      AND x.metric_code=f.accel_code AND x.metric_window=f.metric_window
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
SELECT * EXCLUDE (current_refs,prior_refs,registered_accel),
       CASE WHEN status='registered_candidate_lineage_unverified' THEN registered_accel END AS acceleration,
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
