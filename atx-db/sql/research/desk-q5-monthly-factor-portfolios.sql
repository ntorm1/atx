-- Q5: monthly observation of existing FQ1/FQ2 frozen pre-label portfolios.
-- Parameters: cutoff TIMESTAMP, build_run_id/evaluation_run_id/signal_id VARCHAR,
-- start_date/end_date DATE. Defaults 2015-01-01..2026-12-31 in runner.
-- Reuse FQ2 next-session-close entry, adjusted-price 21/63 SESSION labels,
-- terminal stitching, exclusions and per-decile attrition. Never rerank survivors.
-- Each requested month/horizon survives even with no manifest/calendar/labels.
-- A price gap ending mid-month is flagged, never treated as that month's end.
-- These are overlapping cohort returns, not the P&L of a monthly traded book.
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
), report AS (
    SELECT x.*,o.* EXCLUDE (month_start,horizon_sessions),b.panel_sha256,e.result_sha256,e.label_source,
      json_extract_string(e.config_json,'$.label_version') AS label_version,
      b.blockers_json AS build_blockers,e.blockers_json AS evaluation_blockers,
      CASE WHEN x.month_start>x.cutoff::DATE THEN 'future_month'
           WHEN x.month_end>=x.cutoff::DATE THEN 'month_not_closed_at_cutoff'
           WHEN b.run_id IS NULL THEN 'build_manifest_missing'
           WHEN b.status<>'complete' THEN 'build_not_complete'
           WHEN b.as_of_date>x.cutoff::DATE THEN 'build_snapshot_after_cutoff'
           WHEN f.signal_id IS NULL THEN 'signal_not_in_frozen_build'
           WHEN e.run_id IS NULL THEN 'evaluation_manifest_missing'
           WHEN e.status<>'complete' OR e.build_run_id<>b.run_id
             OR e.build_sha256 IS DISTINCT FROM b.panel_sha256
             OR e.as_of_date>x.cutoff::DATE OR e.run_at>x.cutoff THEN 'evaluation_manifest_mismatch'
           -- v2 labels realize a halt-gap delisting loss at the first absent session (R3a);
           -- sealed v1 runs stay readable: their unstitched halt windows surface as label attrition.
           WHEN json_extract_string(e.config_json,'$.evaluation_version') IS DISTINCT FROM 'fq2_v2'
             OR coalesce(json_extract_string(e.config_json,'$.label_version'),'')
                NOT IN ('forward_return_publication_v1','forward_return_publication_v2')
             THEN 'unsupported_evaluation_contract'
           WHEN x.decision_date IS NULL THEN 'observed_calendar_missing'
           -- Weekend+holiday leaves <=3 days (2015-2026 NYSE); more means the
           -- observed calendar lost the true last session, not a month-end.
           WHEN date_diff('day',x.decision_date,x.month_end)>4 THEN 'observed_month_end_session_missing'
           WHEN o.stored_deciles=0 THEN 'month_end_not_evaluated'
           WHEN o.evaluated_deciles<>10 THEN 'excluded_or_unmatured_month_end'
           WHEN o.eligible_count<>o.labeled_count OR o.missing_labels+o.invalid_labels+o.unsupported_basis_labels<>0
             THEN 'label_attrition_no_complete_portfolio_return'
           ELSE 'stored_complete_cohort_digest_validation_required' END AS status
    FROM expected x JOIN observations o USING (month_start,horizon_sessions)
    LEFT JOIN fundamental_signal_runs b ON b.run_id=x.build_run_id
    LEFT JOIN fundamental_signal_definitions f ON f.run_id=b.run_id AND f.signal_id=x.signal_id
    LEFT JOIN fundamental_signal_evaluation_runs e ON e.run_id=x.evaluation_run_id
)
SELECT *,CASE WHEN status='stored_complete_cohort_digest_validation_required'
             THEN q10_return-q1_return END AS complete_cohort_q10_minus_q1,
       false AS production_qualified,
       'FQ1 modeled availability and historical membership; FQ2 observed/policy terminals separate; no digest certification by this SQL' AS qualification
FROM report ORDER BY month_start,horizon_sessions;
