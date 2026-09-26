-- Q5: monthly observation of existing FQ1/FQ2 frozen pre-label portfolios.
-- Parameters: cutoff TIMESTAMP, build_run_id/evaluation_run_id/signal_id VARCHAR,
-- start_date/end_date DATE. Defaults 2015-01-01..2026-12-31 in runner.
-- Reuse FQ2 next-session-close entry, adjusted-price 21/63 SESSION labels,
-- terminal stitching, exclusions and per-decile attrition. Never rerank survivors.
-- Each requested month/horizon survives even with no manifest/calendar/labels.
-- A price gap ending mid-month is flagged, never treated as that month's end.
-- These are overlapping cohort returns, not the P&L of a monthly traded book.
-- Significance gate (ruling fq2_v3): only an fq2_v3 run's robust inference counts
-- (R3a EWC fixed-b p, Holm over the primary 21-session family of the month's split).
-- fq2_v1/fq2_v2 p-values (calendar Bartlett HAC, normal p) over-reject under
-- overlapping labels: readable as legacy_overconfident_p_value, labeled
-- inference_overconfident_legacy, and never pass the gate.
WITH p AS (
    SELECT $cutoff::TIMESTAMP AS cutoff,$build_run_id::VARCHAR AS build_run_id,
      $evaluation_run_id::VARCHAR AS evaluation_run_id,$signal_id::VARCHAR AS signal_id,
      $start_date::DATE AS start_date,$end_date::DATE AS end_date
), months AS (
    SELECT month_start::DATE AS month_start,last_day(month_start)::DATE AS month_end
    FROM p,generate_series(date_trunc('month',p.start_date),date_trunc('month',p.end_date),INTERVAL 1 MONTH) t(month_start)
), calendar AS (
    SELECT DISTINCT m.trade_date FROM market_daily_metrics m,p
    WHERE m.source='atx-db daily market panel v1' AND m.trade_date BETWEEN p.start_date AND p.end_date
      AND m.trade_date<=p.cutoff::DATE AND m.as_of_date<=p.cutoff::DATE AND m.available_at<=p.cutoff
      AND m.available_at<=m.trade_date::TIMESTAMP+INTERVAL 22 HOUR
), decisions AS (
    SELECT m.month_start,m.month_end,max(c.trade_date) AS decision_date
    FROM months m LEFT JOIN calendar c ON c.trade_date BETWEEN m.month_start AND m.month_end
    GROUP BY m.month_start,m.month_end
), expected AS (
    SELECT d.*,h.horizon_sessions,p.* FROM decisions d CROSS JOIN p
    CROSS JOIN (VALUES (21),(63)) h(horizon_sessions)
), observations AS (
    SELECT x.month_start,x.horizon_sessions,count(d.decile) AS stored_deciles,
      count(d.decile) FILTER (WHERE d.status='evaluated' AND d.decile BETWEEN 1 AND 10) AS evaluated_deciles,
      min(d.entry_date) AS entry_date,max(d.expected_end_date) AS expected_end_date,
      sum(d.eligible_count) AS eligible_count,sum(d.labeled_count) AS labeled_count,
      sum(d.missing_count) AS missing_labels,sum(d.invalid_count) AS invalid_labels,
      sum(d.unsupported_basis_count) AS unsupported_basis_labels,
      sum(d.observed_terminal_count) AS observed_terminal_labels,
      sum(d.policy_terminal_count) AS policy_terminal_labels,
      max(d.tied_boundary_count) AS tied_boundary_count,
      max(d.mean_forward_return) FILTER (WHERE d.decile=10) AS q10_return,
      max(d.mean_forward_return) FILTER (WHERE d.decile=1) AS q1_return,
      list(struct_pack(decile := d.decile,status := d.status,n := d.eligible_count,
        labeled := d.labeled_count,mean_return := d.mean_forward_return) ORDER BY d.decile)
        FILTER (WHERE d.decile IS NOT NULL) AS deciles
    FROM expected x LEFT JOIN fundamental_signal_evaluation_deciles d
      ON d.run_id=x.evaluation_run_id AND d.signal_id=x.signal_id
      AND d.decision_date=x.decision_date AND d.horizon_sessions=x.horizon_sessions
    GROUP BY x.month_start,x.horizon_sessions
), manifest AS (
    SELECT x.*,o.* EXCLUDE (month_start,horizon_sessions),b.panel_sha256,e.result_sha256,e.label_source,
      json_extract_string(e.config_json,'$.label_version') AS label_version,
      json_extract_string(e.config_json,'$.evaluation_version') AS inference_version,
      b.blockers_json AS build_blockers,e.blockers_json AS evaluation_blockers,
      CASE WHEN b.run_id IS NULL THEN 'build_manifest_missing'
           WHEN b.status<>'complete' THEN 'build_not_complete'
           WHEN b.as_of_date>x.cutoff::DATE THEN 'build_snapshot_after_cutoff'
           WHEN f.signal_id IS NULL THEN 'signal_not_in_frozen_build'
           WHEN e.run_id IS NULL THEN 'evaluation_manifest_missing'
           WHEN e.status<>'complete' OR e.build_run_id<>b.run_id
             OR e.build_sha256 IS DISTINCT FROM b.panel_sha256
             OR e.as_of_date>x.cutoff::DATE OR e.run_at>x.cutoff THEN 'evaluation_manifest_mismatch'
           -- v2 labels realize a halt-gap delisting loss at the first absent session (R3a);
           -- sealed v1 runs stay readable: their unstitched halt windows surface as label attrition.
           -- fq2_v2 and fq2_v3 share the observation contract; they differ only in inference.
           WHEN coalesce(json_extract_string(e.config_json,'$.evaluation_version'),'') NOT IN ('fq2_v2','fq2_v3')
             OR coalesce(json_extract_string(e.config_json,'$.label_version'),'')
                NOT IN ('forward_return_publication_v1','forward_return_publication_v2')
             THEN 'unsupported_evaluation_contract'
           END AS contract_failure,
      -- The FQ2 split of the month's decision session (train<=2020, validation 2021..2023, holdout>=2024).
      CASE WHEN x.decision_date IS NULL THEN NULL WHEN year(x.decision_date)<=2020 THEN 'train'
           WHEN year(x.decision_date)<=2023 THEN 'validation' ELSE 'holdout' END AS hypothesis_split
    FROM expected x JOIN observations o USING (month_start,horizon_sessions)
    LEFT JOIN fundamental_signal_runs b ON b.run_id=x.build_run_id
    LEFT JOIN fundamental_signal_definitions f ON f.run_id=b.run_id AND f.signal_id=x.signal_id
    LEFT JOIN fundamental_signal_evaluation_runs e ON e.run_id=x.evaluation_run_id
), report AS (
    SELECT m.*,
      CASE WHEN m.month_start>m.cutoff::DATE THEN 'future_month'
           WHEN m.month_end>=m.cutoff::DATE THEN 'month_not_closed_at_cutoff'
           WHEN m.contract_failure IS NOT NULL THEN m.contract_failure
           WHEN m.decision_date IS NULL THEN 'observed_calendar_missing'
           -- Weekend+holiday leaves <=3 days (2015-2026 NYSE); more means the
           -- observed calendar lost the true last session, not a month-end.
           WHEN date_diff('day',m.decision_date,m.month_end)>4 THEN 'observed_month_end_session_missing'
           WHEN m.stored_deciles=0 THEN 'month_end_not_evaluated'
           WHEN m.evaluated_deciles<>10 THEN 'excluded_or_unmatured_month_end'
           WHEN m.eligible_count<>m.labeled_count OR m.missing_labels+m.invalid_labels+m.unsupported_basis_labels<>0
             THEN 'label_attrition_no_complete_portfolio_return'
           ELSE 'stored_complete_cohort_digest_validation_required' END AS status,
      CASE WHEN m.inference_version='fq2_v3' THEN 'ewc_fixed_b_robust'
           WHEN m.inference_version IN ('fq2_v1','fq2_v2') THEN 'inference_overconfident_legacy'
           WHEN m.inference_version IS NULL THEN 'no_inference'
           ELSE 'unsupported_inference_version' END AS inference_status,
      s.primary_hypothesis,
      CASE WHEN m.inference_version='fq2_v3' THEN s.p_value END AS robust_p_value,
      CASE WHEN m.inference_version='fq2_v3' THEN s.holm_p_value END AS holm_robust_p_value,
      CASE WHEN m.inference_version IN ('fq2_v1','fq2_v2') THEN s.p_value END AS legacy_overconfident_p_value,
      CASE WHEN m.contract_failure IS NOT NULL THEN 'evaluation_contract_failed'
           WHEN m.inference_version IS DISTINCT FROM 'fq2_v3' THEN 'inference_overconfident_legacy'
           WHEN s.run_id IS NULL THEN 'hypothesis_summary_missing'
           WHEN NOT s.primary_hypothesis THEN 'secondary_horizon_not_gated'
           WHEN s.holm_p_value IS NULL THEN 'robust_inference_untestable'
           WHEN s.holm_p_value<=0.05 THEN 'robust_holm_pass'
           ELSE 'robust_holm_not_significant' END AS significance_gate_status
    FROM manifest m
    LEFT JOIN fundamental_signal_evaluation_summaries s
      ON s.run_id=m.evaluation_run_id AND s.signal_id=m.signal_id
      AND s.horizon_sessions=m.horizon_sessions AND s.split=m.hypothesis_split
)
SELECT * EXCLUDE (contract_failure),
       CASE WHEN status='stored_complete_cohort_digest_validation_required'
             THEN q10_return-q1_return END AS complete_cohort_q10_minus_q1,
       significance_gate_status='robust_holm_pass' AS significance_gate_pass,
       false AS production_qualified,
       'FQ1 modeled availability and historical membership; FQ2 observed/policy terminals separate; no digest certification by this SQL; significance only from fq2_v3 robust Holm p' AS qualification
FROM report ORDER BY month_start,horizon_sessions;
