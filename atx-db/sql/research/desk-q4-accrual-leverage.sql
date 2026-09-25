-- Q4: issuer-grain combined screen, using the existing registry exactly.
-- Parameter: cutoff TIMESTAMP. All latest anchors, including incomplete ones.
-- Example cutoffs: Sloan<=.10, NOA/lag assets<=.8, debt/EBITDA<=3,
-- interest coverage>=3, net issuance/assets<=0. Not a calibrated alpha rule.
WITH p AS (SELECT $cutoff::TIMESTAMP AS cutoff),
visible AS NOT MATERIALIZED (
    SELECT d.* FROM derived_metric_values d CROSS JOIN p
    WHERE source='atx-db declarative derived metrics v1' AND available_at<=p.cutoff
      AND as_of_date<=p.cutoff::DATE AND period_end<=p.cutoff::DATE
), anchors AS (
    SELECT security_id,max(target_bucket) AS target_bucket FROM visible GROUP BY security_id
), selected AS (
    SELECT d.* FROM visible d JOIN anchors a USING (security_id,target_bucket)
    WHERE d.metric_code IN ('total_accruals','noa_to_assets','net_debt_ebitda','interest_coverage',
                            'net_equity_issuance','ebitda_ttm','total_assets_avg2')
    QUALIFY row_number() OVER (PARTITION BY d.security_id,d.metric_code,d.metric_window,d.target_bucket
        ORDER BY d.available_at DESC,d.derived_value_id DESC)=1
), panel AS (
    SELECT a.security_id AS issuer_owner_id,a.target_bucket,
      max(d.fiscal_period_end) AS fiscal_period_end,max(d.available_at) AS available_at,
      count(DISTINCT d.fiscal_period_end) AS fiscal_endpoint_count,
      max(CASE WHEN d.value_status='valid' AND isfinite(d.value) AND d.metric_code='total_accruals' THEN d.value END) AS sloan_accruals,
      max(CASE WHEN d.value_status='valid' AND isfinite(d.value) AND d.metric_code='noa_to_assets' THEN d.value END) AS noa_to_lagged_assets,
      max(CASE WHEN d.value_status='valid' AND isfinite(d.value) AND d.metric_code='net_debt_ebitda' THEN d.value END) AS net_debt_ebitda,
      max(CASE WHEN d.value_status='valid' AND isfinite(d.value) AND d.metric_code='interest_coverage' THEN d.value END) AS interest_coverage,
      max(CASE WHEN d.value_status='valid' AND isfinite(d.value) AND d.metric_code='net_equity_issuance' THEN d.value END) AS net_equity_issuance,
      max(CASE WHEN d.value_status='valid' AND isfinite(d.value) AND d.metric_code='ebitda_ttm' THEN d.value END) AS ebitda_ttm,
      max(CASE WHEN d.value_status='valid' AND isfinite(d.value) AND d.metric_code='total_assets_avg2' THEN d.value END) AS total_assets_avg2,
      list(struct_pack(metric := d.metric_code,state_id := d.derived_value_id,
          status := d.value_status,history := d.history_status,refs_hash := d.selected_input_refs_hash))
          FILTER (WHERE d.derived_value_id IS NOT NULL) AS input_states
    FROM anchors a LEFT JOIN selected d ON d.security_id=a.security_id AND
      ((d.metric_window='ttm' AND d.metric_code IN
        ('total_accruals','net_debt_ebitda','interest_coverage','net_equity_issuance','ebitda_ttm'))
       OR (d.metric_window='q' AND d.metric_code='noa_to_assets')
       OR (d.metric_window='avg2' AND d.metric_code='total_assets_avg2'))
    GROUP BY a.security_id,a.target_bucket
), classified AS (
    SELECT *,CASE
      WHEN sloan_accruals IS NULL OR noa_to_lagged_assets IS NULL OR net_debt_ebitda IS NULL
        OR interest_coverage IS NULL OR net_equity_issuance IS NULL OR ebitda_ttm IS NULL
        OR total_assets_avg2 IS NULL THEN 'incomplete'
      WHEN fiscal_endpoint_count<>1 OR fiscal_period_end IS NULL THEN 'mixed_or_unknown_fiscal_endpoint'
      WHEN date_diff('day',fiscal_period_end,(SELECT cutoff::DATE FROM p))>200 THEN 'stale_fiscal_endpoint'
      WHEN ebitda_ttm<=0 OR total_assets_avg2<=0 THEN 'invalid_economic_denominator'
      WHEN sloan_accruals<=0.10 AND noa_to_lagged_assets<=0.80 AND net_debt_ebitda<=3
        AND interest_coverage>=3 AND net_equity_issuance<=0 THEN 'candidate_pass'
      ELSE 'candidate_fail' END AS screen_status FROM panel
)
SELECT *,count(*) OVER () AS visible_owner_count,false AS production_qualified,
       'NOA denominator=assets four quarters ago; issuance=cash issued less buybacks / average assets; selected-leaf proof required'
           AS semantics
FROM classified ORDER BY issuer_owner_id;
