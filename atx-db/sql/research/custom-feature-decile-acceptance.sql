-- Desk question: do the eight predeclared price/liquidity features predict
-- 21-session Q10-minus-Q1 returns, including holdout performance after costs?
-- Read the existing evaluator's outputs; do not rerank features or recompute
-- significance here. Always return all eight hypotheses and all three splits.
-- This query is prepared for the first evaluation, not yet executed live.
WITH requested AS (
    SELECT
        'custom-features-build1'::VARCHAR AS build_run_id,
        'custom-features-evaluation1'::VARCHAR AS evaluation_run_id,
        'cf1_v1'::VARCHAR AS feature_version,
        'atx_custom_price_liquidity_v1'::VARCHAR AS expected_feature_source,
        'tbltickerhistory3_10y'::VARCHAR AS expected_price_source,
        '0ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae'
            ::VARCHAR AS expected_source_sha256,
        DATE '2026-09-20' AS snapshot_date,
        TIMESTAMP '2026-09-20 22:00:00' AS evaluation_cutoff,
        21 AS primary_horizon
), hypotheses(feature_id) AS (
    VALUES ('reversal_5'), ('momentum_126_skip_21'),
           ('volatility_scaled_momentum_63'), ('dollar_volume_shock'),
           ('close_location_pressure_21'), ('range_compression'),
           ('liquidity_conditioned_reversal'), ('compression_accumulation')
), splits(split, split_order) AS (
    VALUES ('train', 1), ('validation', 2), ('holdout', 3)
), expected AS (
    SELECT r.*, h.feature_id, s.split, s.split_order
    FROM requested r CROSS JOIN hypotheses h CROSS JOIN splits s
), date_diagnostics AS (
    SELECT d.feature_id, d.split,
           count(DISTINCT d.decision_date) AS considered_dates,
           count(DISTINCT d.decision_date)
               FILTER (WHERE d.status = 'evaluated') AS evaluated_dates,
           count(DISTINCT d.decision_date)
               FILTER (WHERE d.status <> 'evaluated') AS excluded_dates,
           sum(d.missing_count) AS missing_labels_all_dates,
           sum(d.invalid_label_count) AS invalid_labels_all_dates,
           max(d.label_available_at) AS latest_label_available_at
    FROM custom_feature_deciles d CROSS JOIN requested r
    WHERE d.run_id = r.evaluation_run_id
      AND d.horizon_sessions = r.primary_horizon
    GROUP BY d.feature_id, d.split
)
SELECT x.feature_id, x.split, x.primary_horizon AS horizon_sessions,
       CASE
           WHEN b.run_id IS NULL THEN 'build_manifest_missing'
           WHEN b.feature_version <> x.feature_version
             OR b.feature_source <> x.expected_feature_source
             OR b.price_source <> x.expected_price_source
             OR b.source_sha256 <> x.expected_source_sha256
             OR b.as_of_date <> x.snapshot_date
               THEN 'build_manifest_mismatch'
           WHEN f.feature_id IS NULL THEN 'pinned_definition_missing'
           WHEN v.run_id IS NULL THEN 'evaluation_not_run'
           WHEN v.feature_version <> x.feature_version
             OR v.feature_source <> x.expected_feature_source
             OR v.price_source <> x.expected_price_source
             OR v.source_sha256 <> x.expected_source_sha256
             OR v.as_of_date <> x.snapshot_date
             OR v.run_at <> x.evaluation_cutoff
             OR json_extract_string(v.configuration_json, '$.build_run_id')
                    IS DISTINCT FROM x.build_run_id
             OR try_cast(json_extract_string(v.configuration_json, '$.primary_horizon') AS INTEGER)
                    IS DISTINCT FROM x.primary_horizon
               THEN 'evaluation_manifest_mismatch'
           WHEN e.run_id IS NULL THEN 'hypothesis_result_missing'
           WHEN NOT e.is_primary THEN 'primary_horizon_contract_mismatch'
           WHEN d.latest_label_available_at > x.evaluation_cutoff
               THEN 'label_after_evaluation_cutoff'
           WHEN e.spread_dates = 0 THEN 'no_evaluable_spread_dates'
           WHEN e.holm_p_value IS NULL THEN 'inference_unavailable'
           WHEN NOT e.statistically_qualified THEN 'research_screen_not_met'
           WHEN NOT e.production_eligible THEN 'research_screen_met_production_unqualified'
           ELSE 'inspect_production_eligibility_evidence'
       END AS acceptance_status,
       e.spread_dates, e.gross_mean AS gross_q10_minus_q1,
       e.net_10bp, e.net_25bp, e.net_50bp,
       e.hac_lags, e.hac_standard_error, e.z_statistic,
       e.p_value, e.holm_p_value, e.ci95_low, e.ci95_high,
       e.label_coverage, e.eligible_count, e.labeled_count,
       e.terminal_count, e.imputed_count, e.annual_stability_json,
       e.statistically_qualified, e.production_eligible, e.blockers_json,
       d.considered_dates, d.evaluated_dates, d.excluded_dates,
       d.missing_labels_all_dates, d.invalid_labels_all_dates,
       d.latest_label_available_at,
       f.definition_json, x.build_run_id, x.evaluation_run_id,
       b.feature_source AS build_feature_source,
       b.price_source AS build_price_source,
       v.feature_source AS evaluation_feature_source,
       v.price_source AS evaluation_price_source,
       b.source_sha256 AS build_source_sha256,
       v.source_sha256 AS evaluation_source_sha256,
       v.run_at AS evaluation_run_at,
       v.configuration_json AS evaluation_configuration
FROM expected x
LEFT JOIN custom_feature_runs b
    ON b.run_id = x.build_run_id AND b.run_kind = 'build'
LEFT JOIN custom_feature_definitions f
    ON f.feature_version = x.feature_version AND f.feature_id = x.feature_id
LEFT JOIN custom_feature_runs v
    ON v.run_id = x.evaluation_run_id AND v.run_kind = 'evaluate'
LEFT JOIN custom_feature_evaluations e
    ON e.run_id = x.evaluation_run_id AND e.feature_id = x.feature_id
   AND e.horizon_sessions = x.primary_horizon AND e.split = x.split
LEFT JOIN date_diagnostics d ON d.feature_id = x.feature_id AND d.split = x.split
ORDER BY x.feature_id, x.split_order;
