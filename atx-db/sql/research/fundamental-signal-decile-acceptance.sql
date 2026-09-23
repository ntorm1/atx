-- Desk question: which predeclared earnings/quality signals have a positive
-- 21-session Q10-minus-Q1 holdout spread after costs and family correction?
-- Requires migrations 0324/0325. Prepared; not executed on the live warehouse.
-- Inspect stored results for all five hypotheses, all splits and all horizons.
-- This is not the builder's digest validator or a production certification.
WITH requested AS (
    SELECT 'fundamental_signals_build1'::VARCHAR AS build_run_id,
           'fundamental_signals_evaluation1'::VARCHAR AS evaluation_run_id,
           'atx_forward_returns_survivorship_safe_v1'::VARCHAR AS label_source,
           DATE '2026-09-20' AS snapshot_date,
           TIMESTAMP '2026-09-20 22:00:00' AS evaluation_cutoff
), hypotheses(signal_id) AS (
    VALUES ('eps_growth_yoy'), ('operating_margin_change_yoy'),
           ('low_total_accruals'), ('low_net_debt_ebitda'),
           ('fundamental_rank_mix')
), splits(split, split_order) AS (
    VALUES ('train', 1), ('validation', 2), ('holdout', 3)
), horizons(horizon_sessions, horizon_order) AS (
    VALUES (21, 1), (5, 2), (63, 3)
), expected AS (
    SELECT r.*, h.signal_id, s.split, s.split_order,
           n.horizon_sessions, n.horizon_order
    FROM requested r CROSS JOIN hypotheses h CROSS JOIN splits s
    CROSS JOIN horizons n
), attrition AS (
    SELECT d.signal_id, d.split, d.horizon_sessions,
           count(DISTINCT d.decision_date) AS considered_dates,
           count(DISTINCT d.decision_date)
               FILTER (WHERE d.status = 'evaluated') AS evaluated_dates,
           count(DISTINCT d.decision_date)
               FILTER (WHERE d.status <> 'evaluated') AS excluded_dates,
           sum(d.missing_count) AS missing_labels,
           sum(d.invalid_count) AS invalid_labels,
           sum(d.unsupported_basis_count) AS unsupported_basis_labels
    FROM fundamental_signal_evaluation_deciles d CROSS JOIN requested r
    WHERE d.run_id = r.evaluation_run_id
    GROUP BY d.signal_id, d.split, d.horizon_sessions
), report AS (
    SELECT x.signal_id, x.split, x.split_order, x.horizon_sessions,
           x.horizon_order, x.horizon_sessions = 21 AS primary_hypothesis,
           CASE
               WHEN b.run_id IS NULL THEN 'build_manifest_missing'
               WHEN b.status <> 'complete' THEN 'build_' || b.status
               WHEN b.as_of_date <> x.snapshot_date THEN 'build_snapshot_mismatch'
               WHEN f.signal_id IS NULL THEN 'hypothesis_missing'
               WHEN e.run_id IS NULL THEN 'evaluation_manifest_missing'
               WHEN e.build_run_id <> b.run_id
                 OR e.build_sha256 IS DISTINCT FROM b.panel_sha256
                   THEN 'evaluation_build_mismatch'
               WHEN e.as_of_date <> x.snapshot_date
                 OR e.run_at <> x.evaluation_cutoff
                 OR e.label_source <> x.label_source
                   THEN 'evaluation_scope_mismatch'
               WHEN e.status <> 'complete' THEN 'evaluation_' || e.status
               WHEN s.signal_id IS NULL THEN 'summary_missing'
               ELSE 'stored_results'
           END AS report_status,
           s.status AS statistical_status,
           s.spread_dates, s.gross_mean, s.ci95_low, s.ci95_high,
           s.hac_standard_error, s.p_value, s.holm_p_value,
           s.net_10bp, s.net_25bp, s.net_50bp,
           s.eligible_count, s.labeled_count, s.label_coverage,
           s.observed_terminal_share, s.policy_terminal_share,
           s.annual_json, s.candidate AS stored_statistical_candidate,
           s.production_eligible AS stored_production_eligible,
           s.blockers_json,
           a.considered_dates, a.evaluated_dates, a.excluded_dates,
           a.missing_labels, a.invalid_labels, a.unsupported_basis_labels,
           b.panel_sha256, e.sample_sha256, e.result_sha256
    FROM expected x
    LEFT JOIN fundamental_signal_runs b ON b.run_id = x.build_run_id
    LEFT JOIN fundamental_signal_definitions f
      ON f.run_id = b.run_id AND f.signal_id = x.signal_id
    LEFT JOIN fundamental_signal_evaluation_runs e
      ON e.run_id = x.evaluation_run_id
    LEFT JOIN fundamental_signal_evaluation_summaries s
      ON s.run_id = e.run_id AND s.signal_id = x.signal_id
     AND s.split = x.split AND s.horizon_sessions = x.horizon_sessions
    LEFT JOIN attrition a
      ON a.signal_id = x.signal_id AND a.split = x.split
     AND a.horizon_sessions = x.horizon_sessions
)
SELECT * EXCLUDE (split_order, horizon_order),
       report_status = 'stored_results'
           AND coalesce(stored_statistical_candidate, false) AS candidate_in_requested_run
FROM report
ORDER BY signal_id, split_order, horizon_order;
